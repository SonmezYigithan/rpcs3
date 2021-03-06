﻿#pragma once

#include "Utilities/GSL.h"
#include "Emu/Memory/vm.h"
#include "../GCM.h"
#include "../rsx_utils.h"
#include <list>

namespace
{
	template <typename T>
	gsl::span<T> as_const_span(gsl::span<const gsl::byte> unformated_span)
	{
		return{ (T*)unformated_span.data(), ::narrow<int>(unformated_span.size_bytes() / sizeof(T)) };
	}
}

namespace rsx
{
	namespace utility
	{
		std::vector<u8> get_rtt_indexes(surface_target color_target);
		size_t get_aligned_pitch(surface_color_format format, u32 width);
		size_t get_packed_pitch(surface_color_format format, u32 width);
	}

	template <typename surface_type>
	struct surface_overlap_info_t
	{
		surface_type surface = nullptr;
		u32 base_address = 0;
		bool is_depth = false;
		bool is_clipped = false;

		u16 src_x = 0;
		u16 src_y = 0;
		u16 dst_x = 0;
		u16 dst_y = 0;
		u16 width = 0;
		u16 height = 0;
	};

	struct surface_format_info
	{
		u32 surface_width;
		u32 surface_height;
		u16 native_pitch;
		u16 rsx_pitch;
		u8 bpp;
	};

	template <typename image_storage_type>
	struct surface_hierachy_info
	{
		struct memory_overlap_t
		{
			image_storage_type _ref;
			u32 memory_address;
			u32 x;
			u32 y;
			u32 w;
			u32 h;
		};

		u32 memory_address;
		u32 memory_range;
		image_storage_type memory_contents;

		std::vector<memory_overlap_t> overlapping_set;
	};

	template <typename image_storage_type>
	struct render_target_descriptor
	{
		u64 last_use_tag = 0;         // tag indicating when this block was last confirmed to have been written to
		std::array<std::pair<u32, u64>, 5> memory_tag_samples;

		bool dirty = false;
		image_storage_type old_contents = nullptr;
		rsx::surface_antialiasing read_aa_mode = rsx::surface_antialiasing::center_1_sample;

		GcmTileInfo *tile = nullptr;
		rsx::surface_antialiasing write_aa_mode = rsx::surface_antialiasing::center_1_sample;

		virtual image_storage_type get_surface() = 0;
		virtual u16 get_surface_width() const = 0;
		virtual u16 get_surface_height() const = 0;
		virtual u16 get_rsx_pitch() const = 0;
		virtual u16 get_native_pitch() const = 0;
		virtual bool is_depth_surface() const = 0;

		void save_aa_mode()
		{
			read_aa_mode = write_aa_mode;
			write_aa_mode = rsx::surface_antialiasing::center_1_sample;
		}

		void reset_aa_mode()
		{
			write_aa_mode = read_aa_mode = rsx::surface_antialiasing::center_1_sample;
		}

		bool test() const
		{
			if (dirty)
			{
				// TODO
				// Should RCB or mem-sync (inherit previous mem) to init memory
				LOG_TODO(RSX, "Resource used before memory initialization");
			}

			// Tags are tested in an X pattern
			for (const auto &tag : memory_tag_samples)
			{
				if (!tag.first)
					break;

				if (tag.second != *reinterpret_cast<u64*>(vm::g_sudo_addr + tag.first))
					return false;
			}

			return true;
		}

		template<typename T>
		void set_old_contents(T* other)
		{
			if (!other || other->get_rsx_pitch() != this->get_rsx_pitch())
			{
				old_contents = nullptr;
				return;
			}

			old_contents = other;
		}

		void queue_tag(u32 address)
		{
			for (int i = 0; i < memory_tag_samples.size(); ++i)
			{
				if (LIKELY(i))
					memory_tag_samples[i].first = 0;
				else
					memory_tag_samples[i].first = address; // Top left
			}

			const u32 pitch = get_native_pitch();
			if (UNLIKELY(pitch < 16))
			{
				// Not enough area to gather samples if pitch is too small
				return;
			}

			// Top right corner
			memory_tag_samples[1].first = address + pitch - 8;

			if (const u32 h = get_surface_height(); h > 1)
			{
				// Last row
				const u32 pitch2 = get_rsx_pitch();
				const u32 last_row_offset = pitch2 * (h - 1);
				memory_tag_samples[2].first = address + last_row_offset;              // Bottom left corner
				memory_tag_samples[3].first = address + last_row_offset + pitch - 8;  // Bottom right corner

				// Centroid
				const u32 center_row_offset = pitch2 * (h / 2);
				memory_tag_samples[4].first = address + center_row_offset + pitch / 2;
			}
		}

		void sync_tag()
		{
			for (auto &tag : memory_tag_samples)
			{
				if (!tag.first)
					break;

				tag.second = *reinterpret_cast<u64*>(vm::g_sudo_addr + tag.first);
			}
		}

		void on_write(u64 write_tag = 0)
		{
			if (write_tag)
			{
				// Update use tag if requested
				last_use_tag = write_tag;
			}

			// Tag unconditionally without introducing new data
			sync_tag();

			read_aa_mode = write_aa_mode;
			dirty = false;
			old_contents = nullptr;
		}
	};

	/**
	 * Helper for surface (ie color and depth stencil render target) management.
	 * It handles surface creation and storage. Backend should only retrieve pointer to surface.
	 * It provides 2 methods get_texture_from_*_if_applicable that should be used when an app
	 * wants to sample a previous surface.
	 * Please note that the backend is still responsible for creating framebuffer/descriptors
	 * and need to inform surface_store everytime surface format/size/addresses change.
	 *
	 * Since it's a template it requires a trait with the followings:
	 * - type surface_storage_type which is a structure containing texture.
	 * - type surface_type which is a pointer to storage_type or a reference.
	 * - type command_list_type that can be void for backend without command list
	 * - type download_buffer_object used by issue_download_command and map_downloaded_buffer functions to handle sync
	 *
	 * - a member function static surface_type(const surface_storage_type&) that returns underlying surface pointer from a storage type.
	 * - 2 member functions static surface_storage_type create_new_surface(u32 address, Surface_color_format/Surface_depth_format format, size_t width, size_t height,...)
	 * used to create a new surface_storage_type holding surface from passed parameters.
	 * - a member function static prepare_rtt_for_drawing(command_list, surface_type) that makes a sampleable surface a color render target one.
	 * - a member function static prepare_rtt_for_drawing(command_list, surface_type) that makes a render target surface a sampleable one.
	 * - a member function static prepare_ds_for_drawing that does the same for depth stencil surface.
	 * - a member function static prepare_ds_for_sampling that does the same for depth stencil surface.
	 * - a member function static bool rtt_has_format_width_height(const surface_storage_type&, Surface_color_format surface_color_format, size_t width, size_t height)
	 * that checks if the given surface has the given format and size
	 * - a member function static bool ds_has_format_width_height that does the same for ds
	 * - a member function static download_buffer_object issue_download_command(surface_type, Surface_color_format color_format, size_t width, size_t height,...)
	 * that generates command to download the given surface to some mappable buffer.
	 * - a member function static issue_depth_download_command that does the same for depth surface
	 * - a member function static issue_stencil_download_command that does the same for stencil surface
	 * - a member function gsl::span<const gsl::byte> map_downloaded_buffer(download_buffer_object, ...) that maps a download_buffer_object
	 * - a member function static unmap_downloaded_buffer that unmaps it.
	 */
	template<typename Traits>
	struct surface_store
	{
		template<typename T, typename U>
		void copy_pitched_src_to_dst(gsl::span<T> dest, gsl::span<const U> src, size_t src_pitch_in_bytes, size_t width, size_t height)
		{
			for (int row = 0; row < height; row++)
			{
				for (unsigned col = 0; col < width; col++)
					dest[col] = src[col];
				src = src.subspan(src_pitch_in_bytes / sizeof(U));
				dest = dest.subspan(width);
			}
		}

		constexpr u32 get_aa_factor_v(surface_antialiasing aa_mode)
		{
			switch (aa_mode)
			{
			case surface_antialiasing::center_1_sample:
			case surface_antialiasing::diagonal_centered_2_samples:
				return 1;
			default:
				return 2;
			};
		}

	public:
		using surface_storage_type = typename Traits::surface_storage_type;
		using surface_type = typename Traits::surface_type;
		using command_list_type = typename Traits::command_list_type;
		using download_buffer_object = typename Traits::download_buffer_object;
		using surface_overlap_info = surface_overlap_info_t<surface_type>;

	protected:
		std::unordered_map<u32, surface_storage_type> m_render_targets_storage = {};
		std::unordered_map<u32, surface_storage_type> m_depth_stencil_storage = {};

		rsx::address_range m_render_targets_memory_range;
		rsx::address_range m_depth_stencil_memory_range;

	public:
		std::array<std::tuple<u32, surface_type>, 4> m_bound_render_targets = {};
		std::tuple<u32, surface_type> m_bound_depth_stencil = {};

		std::list<surface_storage_type> invalidated_resources;
		std::vector<surface_hierachy_info<surface_type>> m_memory_tree;
		u64 cache_tag = 0ull;
		u64 write_tag = 0ull;
		u64 memory_tag = 0ull;

		surface_store() = default;
		~surface_store() = default;
		surface_store(const surface_store&) = delete;

	private:
		void generate_render_target_memory_tree()
		{
			auto process_entry = [](surface_hierachy_info<surface_type>& block_info,
				const surface_format_info& info,
				u32 memory_address, u32 memory_end,
				u32 address, surface_type surface)
			{
				if (address <= memory_address) // also intentionally fails on self-test
					return;

				if (address >= memory_end)
					return;

				surface_format_info info2{};
				Traits::get_surface_info(surface, &info2);
				const auto offset = (address - memory_address);
				const auto offset_y = (offset / info.rsx_pitch);
				const auto offset_x = (offset % info.rsx_pitch) / info.bpp;
				const auto pitch2 = info2.bpp * info2.surface_width;

				const bool fits_w = ((offset % info.rsx_pitch) + pitch2) <= info.rsx_pitch;
				const bool fits_h = ((offset_y + info2.surface_height) * info.rsx_pitch) <= (memory_end - memory_address);

				if (fits_w && fits_h)
				{
					typename surface_hierachy_info<surface_type>::memory_overlap_t overlap{};
					overlap._ref = surface;
					overlap.memory_address = address;
					overlap.x = offset_x;
					overlap.y = offset_y;
					overlap.w = info2.surface_width;
					overlap.h = info2.surface_height;

					block_info.overlapping_set.push_back(overlap);
				}
				else
				{
					// TODO
				}
			};

			auto process_block = [this, process_entry](u32 memory_address, surface_type surface)
			{
				surface_hierachy_info<surface_type> block_info;
				surface_format_info info{};
				Traits::get_surface_info(surface, &info);
				const auto memory_end = memory_address + (info.rsx_pitch * info.surface_height);

				for (const auto &rtt : m_render_targets_storage)
				{
					process_entry(block_info, info, memory_address, memory_end, rtt.first, Traits::get(rtt.second));
				}

				for (const auto &ds : m_depth_stencil_storage)
				{
					process_entry(block_info, info, memory_address, memory_end, ds.first, Traits::get(ds.second));
				}

				if (!block_info.overlapping_set.empty())
				{
					block_info.memory_address = memory_address;
					block_info.memory_range = (memory_end - memory_address);
					block_info.memory_contents = surface;

					m_memory_tree.push_back(block_info);
				}
			};

			for (auto &rtt : m_bound_render_targets)
			{
				if (const auto address = std::get<0>(rtt))
				{
					process_block(address, std::get<1>(rtt));
				}
			}

			if (const auto address = std::get<0>(m_bound_depth_stencil))
			{
				process_block(address, std::get<1>(m_bound_depth_stencil));
			}
		}

	protected:
		/**
		* If render target already exists at address, issue state change operation on cmdList.
		* Otherwise create one with width, height, clearColor info.
		* returns the corresponding render target resource.
		*/
		template <typename ...Args>
		surface_type bind_address_as_render_targets(
			command_list_type command_list,
			u32 address,
			surface_color_format color_format,
			surface_antialiasing antialias,
			size_t width, size_t height, size_t pitch,
			Args&&... extra_params)
		{
			// TODO: Fix corner cases
			// This doesn't take overlapping surface(s) into account.
			surface_storage_type old_surface_storage;
			surface_storage_type new_surface_storage;
			surface_type old_surface = nullptr;
			surface_type new_surface = nullptr;
			surface_type convert_surface = nullptr;

			// Remove any depth surfaces occupying this memory address (TODO: Discard all overlapping range)
			auto aliased_depth_surface = m_depth_stencil_storage.find(address);
			if (aliased_depth_surface != m_depth_stencil_storage.end())
			{
				Traits::notify_surface_invalidated(aliased_depth_surface->second);
				convert_surface = Traits::get(aliased_depth_surface->second);
				invalidated_resources.push_back(std::move(aliased_depth_surface->second));
				m_depth_stencil_storage.erase(aliased_depth_surface);
			}

			auto It = m_render_targets_storage.find(address);
			if (It != m_render_targets_storage.end())
			{
				surface_storage_type &rtt = It->second;
				if (Traits::rtt_has_format_width_height(rtt, color_format, width, height))
				{
					if (Traits::surface_is_pitch_compatible(rtt, pitch))
						Traits::notify_surface_persist(rtt);
					else
						Traits::invalidate_surface_contents(command_list, Traits::get(rtt), nullptr, address, pitch);

					Traits::prepare_rtt_for_drawing(command_list, Traits::get(rtt));
					return Traits::get(rtt);
				}

				old_surface = Traits::get(rtt);
				old_surface_storage = std::move(rtt);
				m_render_targets_storage.erase(address);
			}

			// Range test
			const auto aa_factor_v = get_aa_factor_v(antialias);
			rsx::address_range range = rsx::address_range::start_length(address, u32(pitch * height * aa_factor_v));
			m_render_targets_memory_range = range.get_min_max(m_render_targets_memory_range);

			// Select source of original data if any
			auto contents_to_copy = old_surface == nullptr ? convert_surface : old_surface;

			// Search invalidated resources for a suitable surface
			for (auto It = invalidated_resources.begin(); It != invalidated_resources.end(); It++)
			{
				auto &rtt = *It;
				if (Traits::rtt_has_format_width_height(rtt, color_format, width, height, true))
				{
					new_surface_storage = std::move(rtt);

					if (old_surface)
					{
						//Exchange this surface with the invalidated one
						Traits::notify_surface_invalidated(old_surface_storage);
						rtt = std::move(old_surface_storage);
					}
					else
						//rtt is now empty - erase it
						invalidated_resources.erase(It);

					new_surface = Traits::get(new_surface_storage);
					Traits::invalidate_surface_contents(command_list, new_surface, contents_to_copy, address, pitch);
					Traits::prepare_rtt_for_drawing(command_list, new_surface);
					break;
				}
			}

			if (old_surface != nullptr && new_surface == nullptr)
			{
				//This was already determined to be invalid and is excluded from testing above
				Traits::notify_surface_invalidated(old_surface_storage);
				invalidated_resources.push_back(std::move(old_surface_storage));
			}

			if (new_surface != nullptr)
			{
				//New surface was found among existing surfaces
				m_render_targets_storage[address] = std::move(new_surface_storage);
				return new_surface;
			}

			m_render_targets_storage[address] = Traits::create_new_surface(address, color_format, width, height, pitch, contents_to_copy, std::forward<Args>(extra_params)...);
			return Traits::get(m_render_targets_storage[address]);
		}

		template <typename ...Args>
		surface_type bind_address_as_depth_stencil(
			command_list_type command_list,
			u32 address,
			surface_depth_format depth_format,
			surface_antialiasing antialias,
			size_t width, size_t height, size_t pitch,
			Args&&... extra_params)
		{
			surface_storage_type old_surface_storage;
			surface_storage_type new_surface_storage;
			surface_type old_surface = nullptr;
			surface_type new_surface = nullptr;
			surface_type convert_surface = nullptr;

			// Remove any color surfaces occupying this memory range (TODO: Discard all overlapping surfaces)
			auto aliased_rtt_surface = m_render_targets_storage.find(address);
			if (aliased_rtt_surface != m_render_targets_storage.end())
			{
				Traits::notify_surface_invalidated(aliased_rtt_surface->second);
				convert_surface = Traits::get(aliased_rtt_surface->second);
				invalidated_resources.push_back(std::move(aliased_rtt_surface->second));
				m_render_targets_storage.erase(aliased_rtt_surface);
			}

			auto It = m_depth_stencil_storage.find(address);
			if (It != m_depth_stencil_storage.end())
			{
				surface_storage_type &ds = It->second;
				if (Traits::ds_has_format_width_height(ds, depth_format, width, height))
				{
					if (Traits::surface_is_pitch_compatible(ds, pitch))
						Traits::notify_surface_persist(ds);
					else
						Traits::invalidate_surface_contents(command_list, Traits::get(ds), nullptr, address, pitch);

					Traits::prepare_ds_for_drawing(command_list, Traits::get(ds));
					return Traits::get(ds);
				}

				old_surface = Traits::get(ds);
				old_surface_storage = std::move(ds);
				m_depth_stencil_storage.erase(address);
			}

			// Range test
			const auto aa_factor_v = get_aa_factor_v(antialias);
			rsx::address_range range = rsx::address_range::start_length(address, u32(pitch * height * aa_factor_v));
			m_depth_stencil_memory_range = range.get_min_max(m_depth_stencil_memory_range);

			// Select source of original data if any
			auto contents_to_copy = old_surface == nullptr ? convert_surface : old_surface;

			//Search invalidated resources for a suitable surface
			for (auto It = invalidated_resources.begin(); It != invalidated_resources.end(); It++)
			{
				auto &ds = *It;
				if (Traits::ds_has_format_width_height(ds, depth_format, width, height, true))
				{
					new_surface_storage = std::move(ds);

					if (old_surface)
					{
						//Exchange this surface with the invalidated one
						Traits::notify_surface_invalidated(old_surface_storage);
						ds = std::move(old_surface_storage);
					}
					else
						invalidated_resources.erase(It);

					new_surface = Traits::get(new_surface_storage);
					Traits::prepare_ds_for_drawing(command_list, new_surface);
					Traits::invalidate_surface_contents(command_list, new_surface, contents_to_copy, address, pitch);
					break;
				}
			}

			if (old_surface != nullptr && new_surface == nullptr)
			{
				//This was already determined to be invalid and is excluded from testing above
				Traits::notify_surface_invalidated(old_surface_storage);
				invalidated_resources.push_back(std::move(old_surface_storage));
			}

			if (new_surface != nullptr)
			{
				//New surface was found among existing surfaces
				m_depth_stencil_storage[address] = std::move(new_surface_storage);
				return new_surface;
			}

			m_depth_stencil_storage[address] = Traits::create_new_surface(address, depth_format, width, height, pitch, contents_to_copy, std::forward<Args>(extra_params)...);
			return Traits::get(m_depth_stencil_storage[address]);
		}
	public:
		/**
		 * Update bound color and depth surface.
		 * Must be called everytime surface format, clip, or addresses changes.
		 */
		template <typename ...Args>
		void prepare_render_target(
			command_list_type command_list,
			surface_color_format color_format, surface_depth_format depth_format,
			u32 clip_horizontal_reg, u32 clip_vertical_reg,
			surface_target set_surface_target,
			surface_antialiasing antialias,
			const std::array<u32, 4> &surface_addresses, u32 address_z,
			const std::array<u32, 4> &surface_pitch, u32 zeta_pitch,
			Args&&... extra_params)
		{
			u32 clip_width = clip_horizontal_reg;
			u32 clip_height = clip_vertical_reg;
//			u32 clip_x = clip_horizontal_reg;
//			u32 clip_y = clip_vertical_reg;

			cache_tag = rsx::get_shared_tag();
			m_memory_tree.clear();

			// Make previous RTTs sampleable
			for (std::tuple<u32, surface_type> &rtt : m_bound_render_targets)
			{
				if (std::get<1>(rtt) != nullptr)
					Traits::prepare_rtt_for_sampling(command_list, std::get<1>(rtt));
				rtt = std::make_tuple(0, nullptr);
			}

			// Create/Reuse requested rtts
			for (u8 surface_index : utility::get_rtt_indexes(set_surface_target))
			{
				if (surface_addresses[surface_index] == 0)
					continue;

				m_bound_render_targets[surface_index] = std::make_tuple(surface_addresses[surface_index],
					bind_address_as_render_targets(command_list, surface_addresses[surface_index], color_format, antialias,
						clip_width, clip_height, surface_pitch[surface_index], std::forward<Args>(extra_params)...));
			}

			// Same for depth buffer
			if (std::get<1>(m_bound_depth_stencil) != nullptr)
				Traits::prepare_ds_for_sampling(command_list, std::get<1>(m_bound_depth_stencil));

			m_bound_depth_stencil = std::make_tuple(0, nullptr);

			if (!address_z)
				return;

			m_bound_depth_stencil = std::make_tuple(address_z,
				bind_address_as_depth_stencil(command_list, address_z, depth_format, antialias,
					clip_width, clip_height, zeta_pitch, std::forward<Args>(extra_params)...));
		}

		/**
		 * Search for given address in stored color surface
		 * Return an empty surface_type otherwise.
		 */
		surface_type get_texture_from_render_target_if_applicable(u32 address)
		{
			auto It = m_render_targets_storage.find(address);
			if (It != m_render_targets_storage.end())
				return Traits::get(It->second);
			return surface_type();
		}

		/**
		* Search for given address in stored depth stencil surface
		* Return an empty surface_type otherwise.
		*/
		surface_type get_texture_from_depth_stencil_if_applicable(u32 address)
		{
			auto It = m_depth_stencil_storage.find(address);
			if (It != m_depth_stencil_storage.end())
				return Traits::get(It->second);
			return surface_type();
		}

		surface_type get_surface_at(u32 address)
		{
			auto It = m_render_targets_storage.find(address);
			if (It != m_render_targets_storage.end())
				return Traits::get(It->second);

			auto _It = m_depth_stencil_storage.find(address);
			if (_It != m_depth_stencil_storage.end())
				return Traits::get(_It->second);

			fmt::throw_exception("Unreachable" HERE);
		}

		/**
		 * Get bound color surface raw data.
		 */
		template <typename... Args>
		std::array<std::vector<gsl::byte>, 4> get_render_targets_data(
			surface_color_format color_format, size_t width, size_t height,
			Args&& ...args
			)
		{
			std::array<download_buffer_object, 4> download_data = {};

			// Issue download commands
			for (int i = 0; i < 4; i++)
			{
				if (std::get<0>(m_bound_render_targets[i]) == 0)
					continue;

				surface_type surface_resource = std::get<1>(m_bound_render_targets[i]);
				download_data[i] = std::move(
					Traits::issue_download_command(surface_resource, color_format, width, height, std::forward<Args&&>(args)...)
					);
			}

			std::array<std::vector<gsl::byte>, 4> result = {};

			// Sync and copy data
			for (int i = 0; i < 4; i++)
			{
				if (std::get<0>(m_bound_render_targets[i]) == 0)
					continue;

				gsl::span<const gsl::byte> raw_src = Traits::map_downloaded_buffer(download_data[i], std::forward<Args&&>(args)...);

				size_t src_pitch = utility::get_aligned_pitch(color_format, ::narrow<u32>(width));
				size_t dst_pitch = utility::get_packed_pitch(color_format, ::narrow<u32>(width));

				result[i].resize(dst_pitch * height);

				// Note: MSVC + GSL doesn't support span<byte> -> span<T> for non const span atm
				// thus manual conversion
				switch (color_format)
				{
				case surface_color_format::a8b8g8r8:
				case surface_color_format::x8b8g8r8_o8b8g8r8:
				case surface_color_format::x8b8g8r8_z8b8g8r8:
				case surface_color_format::a8r8g8b8:
				case surface_color_format::x8r8g8b8_o8r8g8b8:
				case surface_color_format::x8r8g8b8_z8r8g8b8:
				case surface_color_format::x32:
				{
					gsl::span<be_t<u32>> dst_span{ (be_t<u32>*)result[i].data(), ::narrow<int>(dst_pitch * height / sizeof(be_t<u32>)) };
					copy_pitched_src_to_dst(dst_span, as_const_span<const u32>(raw_src), src_pitch, width, height);
					break;
				}
				case surface_color_format::b8:
				{
					gsl::span<u8> dst_span{ (u8*)result[i].data(), ::narrow<int>(dst_pitch * height / sizeof(u8)) };
					copy_pitched_src_to_dst(dst_span, as_const_span<const u8>(raw_src), src_pitch, width, height);
					break;
				}
				case surface_color_format::g8b8:
				case surface_color_format::r5g6b5:
				case surface_color_format::x1r5g5b5_o1r5g5b5:
				case surface_color_format::x1r5g5b5_z1r5g5b5:
				{
					gsl::span<be_t<u16>> dst_span{ (be_t<u16>*)result[i].data(), ::narrow<int>(dst_pitch * height / sizeof(be_t<u16>)) };
					copy_pitched_src_to_dst(dst_span, as_const_span<const u16>(raw_src), src_pitch, width, height);
					break;
				}
				// Note : may require some big endian swap
				case surface_color_format::w32z32y32x32:
				{
					gsl::span<u128> dst_span{ (u128*)result[i].data(), ::narrow<int>(dst_pitch * height / sizeof(u128)) };
					copy_pitched_src_to_dst(dst_span, as_const_span<const u128>(raw_src), src_pitch, width, height);
					break;
				}
				case surface_color_format::w16z16y16x16:
				{
					gsl::span<u64> dst_span{ (u64*)result[i].data(), ::narrow<int>(dst_pitch * height / sizeof(u64)) };
					copy_pitched_src_to_dst(dst_span, as_const_span<const u64>(raw_src), src_pitch, width, height);
					break;
				}

				}
				Traits::unmap_downloaded_buffer(download_data[i], std::forward<Args&&>(args)...);
			}
			return result;
		}

		/**
		 * Get bound color surface raw data.
		 */
		template <typename... Args>
		std::array<std::vector<gsl::byte>, 2> get_depth_stencil_data(
			surface_depth_format depth_format, size_t width, size_t height,
			Args&& ...args
			)
		{
			std::array<std::vector<gsl::byte>, 2> result = {};
			if (std::get<0>(m_bound_depth_stencil) == 0)
				return result;
			size_t row_pitch = align(width * 4, 256);

			download_buffer_object stencil_data = {};
			download_buffer_object depth_data = Traits::issue_depth_download_command(std::get<1>(m_bound_depth_stencil), depth_format, width, height, std::forward<Args&&>(args)...);
			if (depth_format == surface_depth_format::z24s8)
				stencil_data = std::move(Traits::issue_stencil_download_command(std::get<1>(m_bound_depth_stencil), width, height, std::forward<Args&&>(args)...));

			gsl::span<const gsl::byte> depth_buffer_raw_src = Traits::map_downloaded_buffer(depth_data, std::forward<Args&&>(args)...);
			if (depth_format == surface_depth_format::z16)
			{
				result[0].resize(width * height * 2);
				gsl::span<u16> dest{ (u16*)result[0].data(), ::narrow<int>(width * height) };
				copy_pitched_src_to_dst(dest, as_const_span<const u16>(depth_buffer_raw_src), row_pitch, width, height);
			}
			if (depth_format == surface_depth_format::z24s8)
			{
				result[0].resize(width * height * 4);
				gsl::span<u32> dest{ (u32*)result[0].data(), ::narrow<int>(width * height) };
				copy_pitched_src_to_dst(dest, as_const_span<const u32>(depth_buffer_raw_src), row_pitch, width, height);
			}
			Traits::unmap_downloaded_buffer(depth_data, std::forward<Args&&>(args)...);

			if (depth_format == surface_depth_format::z16)
				return result;

			gsl::span<const gsl::byte> stencil_buffer_raw_src = Traits::map_downloaded_buffer(stencil_data, std::forward<Args&&>(args)...);
			result[1].resize(width * height);
			gsl::span<u8> dest{ (u8*)result[1].data(), ::narrow<int>(width * height) };
			copy_pitched_src_to_dst(dest, as_const_span<const u8>(stencil_buffer_raw_src), align(width, 256), width, height);
			Traits::unmap_downloaded_buffer(stencil_data, std::forward<Args&&>(args)...);
			return result;
		}

		/**
		 * Moves a single surface from surface storage to invalidated surface store.
		 * Can be triggered by the texture cache's blit functionality when formats do not match
		 */
		void invalidate_single_surface(surface_type surface, bool depth)
		{
			if (!depth)
			{
				for (auto It = m_render_targets_storage.begin(); It != m_render_targets_storage.end(); It++)
				{
					const auto address = It->first;
					const auto ref = Traits::get(It->second);

					if (surface == ref)
					{
						Traits::notify_surface_invalidated(It->second);
						invalidated_resources.push_back(std::move(It->second));
						m_render_targets_storage.erase(It);

						cache_tag = rsx::get_shared_tag();
						return;
					}
				}
			}
			else
			{
				for (auto It = m_depth_stencil_storage.begin(); It != m_depth_stencil_storage.end(); It++)
				{
					const auto address = It->first;
					const auto ref = Traits::get(It->second);

					if (surface == ref)
					{
						Traits::notify_surface_invalidated(It->second);
						invalidated_resources.push_back(std::move(It->second));
						m_depth_stencil_storage.erase(It);

						cache_tag = rsx::get_shared_tag();
						return;
					}
				}
			}
		}

		/**
		 * Invalidates surface that exists at an address
		 */
		void invalidate_surface_address(u32 addr, bool depth)
		{
			if (address_is_bound(addr))
			{
				LOG_ERROR(RSX, "Cannot invalidate a currently bound render target!");
				return;
			}

			if (!depth)
			{
				auto It = m_render_targets_storage.find(addr);
				if (It != m_render_targets_storage.end())
				{
					Traits::notify_surface_invalidated(It->second);
					invalidated_resources.push_back(std::move(It->second));
					m_render_targets_storage.erase(It);

					cache_tag = rsx::get_shared_tag();
					return;
				}
			}
			else
			{
				auto It = m_depth_stencil_storage.find(addr);
				if (It != m_depth_stencil_storage.end())
				{
					Traits::notify_surface_invalidated(It->second);
					invalidated_resources.push_back(std::move(It->second));
					m_depth_stencil_storage.erase(It);

					cache_tag = rsx::get_shared_tag();
					return;
				}
			}
		}

		bool address_is_bound(u32 address) const
		{
			for (auto &surface : m_bound_render_targets)
			{
				const u32 bound_address = std::get<0>(surface);
				if (bound_address == address)
					return true;
			}

			if (std::get<0>(m_bound_depth_stencil) == address)
				return true;

			return false;
		}

		template <typename commandbuffer_type>
		std::vector<surface_overlap_info> get_merged_texture_memory_region(commandbuffer_type& cmd, u32 texaddr, u32 required_width, u32 required_height, u32 required_pitch)
		{
			std::vector<surface_overlap_info> result;
			std::vector<std::pair<u32, bool>> dirty;
			const u32 limit = texaddr + (required_pitch * required_height);

			auto process_list_function = [&](std::unordered_map<u32, surface_storage_type>& data, bool is_depth)
			{
				for (auto &tex_info : data)
				{
					auto this_address = std::get<0>(tex_info);
					if (this_address >= limit)
						continue;

					auto surface = std::get<1>(tex_info).get();
					const auto pitch = surface->get_rsx_pitch();
					if (!rsx::pitch_compatible(surface, required_pitch, required_height))
						continue;

					const u16 scale_x = surface->read_aa_mode > rsx::surface_antialiasing::center_1_sample? 2 : 1;
					const u16 scale_y = surface->read_aa_mode > rsx::surface_antialiasing::diagonal_centered_2_samples? 2 : 1;
					const auto texture_size = pitch * surface->get_surface_height() * scale_y;

					if ((this_address + texture_size) <= texaddr)
						continue;

					if (surface->read_barrier(cmd); !surface->test())
					{
						dirty.emplace_back(this_address, is_depth);
						continue;
					}

					surface_overlap_info info;
					info.surface = surface;
					info.base_address = this_address;
					info.is_depth = is_depth;

					surface_format_info surface_info{};
					Traits::get_surface_info(surface, &surface_info);

					if (this_address < texaddr)
					{
						const auto int_required_width = required_width / scale_x;
						const auto int_required_height = required_height / scale_y;

						auto offset = texaddr - this_address;
						info.src_y = (offset / required_pitch) / scale_y;
						info.src_x = (offset % required_pitch) / surface_info.bpp / scale_x;
						info.dst_x = 0;
						info.dst_y = 0;
						info.width = std::min<u32>(int_required_width, surface_info.surface_width - info.src_x);
						info.height = std::min<u32>(int_required_height, surface_info.surface_height - info.src_y);
						info.is_clipped = (info.width < int_required_width || info.height < int_required_height);
					}
					else
					{
						const auto int_surface_width = surface_info.surface_width * scale_x;
						const auto int_surface_height = surface_info.surface_height * scale_y;

						auto offset = this_address - texaddr;
						info.src_x = 0;
						info.src_y = 0;
						info.dst_y = (offset / required_pitch);
						info.dst_x = (offset % required_pitch) / surface_info.bpp;
						info.width = std::min<u32>(int_surface_width, required_width - info.dst_x);
						info.height = std::min<u32>(int_surface_height, required_height - info.dst_y);
						info.is_clipped = (info.width < required_width || info.height < required_height);
						info.width /= scale_x;
						info.height /= scale_y;
					}

					result.push_back(info);
				}
			};

			// Range test helper to quickly discard blocks
			// Fortunately, render targets tend to be clustered anyway
			rsx::address_range test = rsx::address_range::start_end(texaddr, limit-1);

			if (test.overlaps(m_render_targets_memory_range))
			{
				process_list_function(m_render_targets_storage, false);
			}

			if (test.overlaps(m_depth_stencil_memory_range))
			{
				process_list_function(m_depth_stencil_storage, true);
			}

			if (!dirty.empty())
			{
				for (const auto& p : dirty)
				{
					invalidate_surface_address(p.first, p.second);
				}
			}

			if (result.size() > 1)
			{
				std::sort(result.begin(), result.end(), [](const auto &a, const auto &b)
				{
					if (a.surface->last_use_tag == b.surface->last_use_tag)
					{
						const auto area_a = a.width * a.height;
						const auto area_b = b.width * b.height;

						return area_a < area_b;
					}

					return a.surface->last_use_tag < b.surface->last_use_tag;
				});
			}

			return result;
		}

		void on_write(u32 address = 0)
		{
			if (!address)
			{
				if (write_tag == cache_tag)
				{
					// Nothing to do
					return;
				}
				else
				{
					write_tag = cache_tag;
				}
			}

			if (memory_tag != cache_tag)
			{
				generate_render_target_memory_tree();
				memory_tag = cache_tag;
			}

			if (!m_memory_tree.empty())
			{
				for (auto &e : m_memory_tree)
				{
					if (address && e.memory_address != address)
					{
						continue;
					}

					for (auto &entry : e.overlapping_set)
					{
						// GPU-side contents changed
						entry._ref->dirty = true;
					}
				}
			}

			for (auto &rtt : m_bound_render_targets)
			{
				if (address && std::get<0>(rtt) != address)
				{
					continue;
				}

				if (auto surface = std::get<1>(rtt))
				{
					surface->on_write(write_tag);
				}
			}

			if (auto ds = std::get<1>(m_bound_depth_stencil))
			{
				if (!address || std::get<0>(m_bound_depth_stencil) == address)
				{
					ds->on_write(write_tag);
				}
			}
		}

		void notify_memory_structure_changed()
		{
			cache_tag = rsx::get_shared_tag();
		}
	};
}
