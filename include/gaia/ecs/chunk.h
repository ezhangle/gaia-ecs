#pragma once
#include "../config/config.h"

#include <cinttypes>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "../cnt/sarray_ext.h"
#include "../config/profiler.h"
#include "../core/utility.h"
#include "archetype_common.h"
#include "chunk_allocator.h"
#include "chunk_header.h"
#include "common.h"
#include "component.h"
#include "component_cache.h"
#include "component_utils.h"
#include "entity.h"

namespace gaia {
	namespace ecs {
		namespace archetype {
			class Chunk final {
			public:
				// TODO: Make this private
				//! Chunk header
				ChunkHeader m_header;

			private:
				//! Pointer to where the chunk data starts.
				//! Data layed out as following:
				//!			1) ComponentVersions[component::ComponentType::CT_Generic]
				//!			2) ComponentVersions[component::ComponentType::CT_Chunk]
				//!     3) ComponentIds[component::ComponentType::CT_Generic]
				//!			4) ComponentIds[component::ComponentType::CT_Chunk]
				//!			5) ComponentOffsets[component::ComponentType::CT_Generic]
				//!			6) ComponentOffsets[component::ComponentType::CT_Chunk]
				//!			7) Entities
				//!			8) Components
				//! Note, root archetypes store only entites, therefore it is fully occupied with entities.
				uint8_t m_data[1];

				GAIA_MSVC_WARNING_PUSH()
				GAIA_MSVC_WARNING_DISABLE(26495)

				Chunk(
						uint32_t archetypeId, uint32_t chunkIndex, uint16_t capacity, uint16_t st, uint32_t& worldVersion,
						const ChunkHeaderOffsets& headerOffsets):
						m_header(archetypeId, chunkIndex, capacity, st, headerOffsets, worldVersion) {
					// Chunk data area consist of memory offsets + component data. Normally. we would initialize it.
					// However, the memory offsets part are all trivial types and components are initialized via their
					// constructors so we do not really need to do anything.
					// const uint32_t sizeBytes = ...;
					// auto* curr = (uint8_t*)&m_data[0] + 1;
					// for (uint32_t i = 0; i < sizeBytes; ++i, (void)++curr) {
					// 	const auto* addr = mem::addressof(*curr);
					// 	(void)new (const_cast<void*>(static_cast<const volatile void*>(addr))) uint8_t;
					// }
				}

				GAIA_MSVC_WARNING_POP()

				void init(
						const cnt::sarray<ComponentIdArray, component::ComponentType::CT_Count>& compIds,
						const cnt::sarray<ComponentOffsetArray, component::ComponentType::CT_Count>& compOffs) {
					m_header.componentCount[component::ComponentType::CT_Generic] =
							(uint8_t)compIds[component::ComponentType::CT_Generic].size();
					m_header.componentCount[component::ComponentType::CT_Chunk] =
							(uint8_t)compIds[component::ComponentType::CT_Chunk].size();

					const auto& cc = ComponentCache::get();

					const auto& compIdsGeneric = compIds[component::ComponentType::CT_Generic];
					for (const auto componentId: compIdsGeneric) {
						const auto& desc = cc.comp_desc(componentId);
						m_header.hasAnyCustomGenericCtor |= (desc.func_ctor != nullptr);
						m_header.hasAnyCustomGenericDtor |= (desc.func_dtor != nullptr);
					}
					const auto& compIdsChunk = compIds[component::ComponentType::CT_Chunk];
					for (const auto componentId: compIdsChunk) {
						const auto& desc = cc.comp_desc(componentId);
						m_header.hasAnyCustomChunkCtor |= (desc.func_ctor != nullptr);
						m_header.hasAnyCustomChunkDtor |= (desc.func_dtor != nullptr);
					}

					// Copy provided component id data to this chunk's data area
					{
						for (uint32_t i = 0; i < component::ComponentType::CT_Count; ++i) {
							auto offset = m_header.offsets.firstByte_ComponentIds[i];
							for (const auto componentId: compIds[i]) {
								// unaligned_ref not necessary because data is aligned
								*(component::ComponentId*)&m_data[offset] = componentId;
								offset += sizeof(component::ComponentId);
							}
						}
					}

					// Copy provided component offset data to this chunk's data area
					{
						for (uint32_t i = 0; i < component::ComponentType::CT_Count; ++i) {
							auto offset = m_header.offsets.firstByte_ComponentOffsets[i];
							for (const auto componentOffset: compOffs[i]) {
								// unaligned_ref not necessary because data is aligned
								*(archetype::ChunkComponentOffset*)&m_data[offset] = componentOffset;
								offset += sizeof(archetype::ChunkComponentOffset);
							}
						}
					}
				}

				/*!
				Returns a read-only span of the component data.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\return Span of read-only component data.
				*/
				template <typename T>
				GAIA_NODISCARD GAIA_FORCEINLINE auto view_inter() const -> decltype(std::span<const uint8_t>{}) {
					using U = typename component::component_type_t<T>::Type;

					if constexpr (std::is_same_v<U, Entity>) {
						return {&data(m_header.offsets.firstByte_EntityData), size()};
					} else {
						static_assert(!std::is_empty_v<U>, "Attempting to get value of an empty component");

						const auto componentId = component::comp_id<T>();
						constexpr auto compType = component::component_type_v<T>;

						// Find at what byte offset the first component of a given type is located
						uint32_t componentIdx = 0;
						const auto offset = find_data_offset(compType, componentId, componentIdx);

						if constexpr (compType == component::ComponentType::CT_Generic) {
							[[maybe_unused]] const auto maxOffset = offset + capacity() * sizeof(U);
							GAIA_ASSERT(maxOffset <= bytes());

							return {&data(offset), size()};
						} else {
							[[maybe_unused]] const auto maxOffset = offset + sizeof(U);
							GAIA_ASSERT(maxOffset <= bytes());

							return {&data(offset), 1};
						}
					}
				}

				/*!
				Returns a read-write span of the component data. Also updates the world version for the component.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\tparam WorldVersionUpdateWanted If true, the world version is updated as a result of the write access
				\return Span of read-write component data.
				*/
				template <typename T, bool WorldVersionUpdateWanted>
				GAIA_NODISCARD GAIA_FORCEINLINE auto view_mut_inter() -> decltype(std::span<uint8_t>{}) {
					using U = typename component::component_type_t<T>::Type;
#if GAIA_COMPILER_MSVC && _MSC_VER <= 1916
					// Workaround for MSVC 2017 bug where it incorrectly evaluates the static assert
					// even in context where it shouldn't.
					// Unfortunatelly, even runtime assert can't be used...
					// GAIA_ASSERT(!std::is_same_v<U, Entity>::value);
#else
					static_assert(!std::is_same_v<U, Entity>);
#endif
					static_assert(!std::is_empty_v<U>, "Attempting to set value of an empty component");

					const auto componentId = component::comp_id<T>();
					constexpr auto compType = component::component_type_v<T>;

					// Find at what byte offset the first component of a given type is located
					uint32_t componentIdx = 0;
					const auto offset = find_data_offset(compType, componentId, componentIdx);

					// Update version number if necessary so we know RW access was used on the chunk
					if constexpr (WorldVersionUpdateWanted)
						this->update_world_version(compType, componentIdx);

					if constexpr (compType == component::ComponentType::CT_Generic) {
						[[maybe_unused]] const auto maxOffset = offset + capacity() * sizeof(U);
						GAIA_ASSERT(maxOffset <= bytes());

						return {&data(offset), size()};
					} else {
						[[maybe_unused]] const auto maxOffset = offset + sizeof(U);
						GAIA_ASSERT(maxOffset <= bytes());

						return {&data(offset), 1};
					}
				}

				/*!
				Returns the value stored in the component \tparam T on \param index in the chunk.
				\warning It is expected the \param index is valid. Undefined behavior otherwise.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\param index Index of entity in the chunk
				\return Value stored in the component if smaller than 8 bytes. Const reference to the value otherwise.
				*/
				template <typename T>
				GAIA_NODISCARD auto comp_inter(uint32_t index) const {
					using U = typename component::component_type_t<T>::Type;
					using RetValueType = decltype(view<T>()[0]);

					GAIA_ASSERT(index < m_header.count);
					if constexpr (sizeof(RetValueType) > 8)
						return (const U&)view<T>()[index];
					else
						return view<T>()[index];
				}

				/*!
				Removes the entity at from the chunk and updates the world versions
				*/
				void remove_last_entity_inter() {
					// Should never be called over an empty chunk
					GAIA_ASSERT(has_entities());
					--m_header.count;
					--m_header.countEnabled;
				}

			public:
				Chunk(const Chunk& chunk) = delete;
				Chunk(Chunk&& chunk) = delete;
				Chunk& operator=(const Chunk& chunk) = delete;
				Chunk& operator=(Chunk&& chunk) = delete;
				~Chunk() = default;

				static uint16_t chunk_total_bytes(uint16_t dataSize) {
					uint16_t header = (uint16_t)sizeof(ChunkHeader) + (uint16_t)MemoryBlockUsableOffset;
					return header + dataSize;
				}

				static uint16_t chunk_data_bytes(uint16_t totalSize) {
					uint16_t header = (uint16_t)sizeof(ChunkHeader) + (uint16_t)MemoryBlockUsableOffset;
					return totalSize - header;
				}

				/*!
				Allocates memory for a new chunk.
				\param chunkIndex Index of this chunk within the parent archetype
				\return Newly allocated chunk
				*/
				static Chunk* create(
						uint32_t archetypeId, uint32_t chunkIndex, uint16_t capacity, uint16_t dataBytes, uint32_t& worldVersion,
						const ChunkHeaderOffsets& offsets,
						const cnt::sarray<ComponentIdArray, component::ComponentType::CT_Count>& compIds,
						const cnt::sarray<ComponentOffsetArray, component::ComponentType::CT_Count>& compOffs) {
					const auto totalBytes = chunk_total_bytes(dataBytes);
					const auto sizeType = detail::ChunkAllocatorImpl::mem_block_size_type(totalBytes);
#if GAIA_ECS_CHUNK_ALLOCATOR
					auto* pChunk = (Chunk*)ChunkAllocator::get().alloc(totalBytes);
					new (pChunk) Chunk(archetypeId, chunkIndex, capacity, sizeType, worldVersion, offsets);
#else
					GAIA_ASSERT(totalBytes <= MaxMemoryBlockSize);
					const auto allocSize = detail::ChunkAllocatorImpl::mem_block_size(sizeType);
					auto* pChunkMem = new uint8_t[allocSize];
					auto* pChunk = new (pChunkMem) Chunk(archetypeId, chunkIndex, capacity, sizeType, worldVersion, offsets);
#endif

					pChunk->init(compIds, compOffs);

					return pChunk;
				}

				/*!
				Releases all memory allocated by \param pChunk.
				\param pChunk Chunk which we want to destroy
				*/
				static void free(Chunk* pChunk) {
					GAIA_ASSERT(pChunk != nullptr);

					// Call destructors for components that need it
					if (pChunk->has_custom_generic_dtor())
						pChunk->call_dtors(component::ComponentType::CT_Generic, 0, pChunk->size());
					if (pChunk->has_custom_chunk_dtor())
						pChunk->call_dtors(component::ComponentType::CT_Chunk, 0, 1);

#if GAIA_ECS_CHUNK_ALLOCATOR
					pChunk->~Chunk();
					ChunkAllocator::get().free(pChunk);
#else
					pChunk->~Chunk();
					auto* pChunkMem = (uint8_t*)pChunk;
					delete pChunkMem;
#endif
				}

				/*!
				Remove the last entity from chunk.
				\param chunksToRemove Container of chunks ready for removal
				*/
				void remove_last_entity(cnt::darray<archetype::Chunk*>& chunksToRemove) {
					GAIA_ASSERT(
							!has_structural_changes() && "Entities can't be removed while their chunk is being iterated "
																					 "(structural changes are forbidden during this time!)");

					remove_last_entity_inter();

					if (!dying() && !has_entities()) {
						// When the chunk is emptied we want it to be removed. We can't do it
						// right away and need to wait for world's GC to be called.
						//
						// However, we need to prevent the following:
						//    1) chunk is emptied, add it to some removal list
						//    2) chunk is reclaimed
						//    3) chunk is emptied, add it to some removal list again
						//
						// Therefore, we have a flag telling us the chunk is already waiting to
						// be removed. The chunk might be reclaimed before GC happens but it
						// simply ignores such requests. This way GC always has at most one
						// record for removal for any given chunk.
						prepare_to_die();

						chunksToRemove.push_back(this);
					}
				}

				//! Updates the version numbers for this chunk.
				void update_versions() {
					update_version(m_header.worldVersion);
					update_world_version(component::ComponentType::CT_Generic);
					update_world_version(component::ComponentType::CT_Chunk);
				}

				/*!
				Returns a read-only entity or component view.
				\warning If \tparam T is a component it is expected it is present. Undefined behavior otherwise.
				\tparam T Component or Entity
				\return Entity of component view with read-only access
				*/
				template <typename T>
				GAIA_NODISCARD auto view() const {
					using U = typename component::component_type_t<T>::Type;

					return mem::auto_view_policy_get<U>{view_inter<T>()};
				}

				/*!
				Returns a mutable entity or component view.
				\warning If \tparam T is a component it is expected it is present. Undefined behavior otherwise.
				\tparam T Component or Entity
				\return Entity or component view with read-write access
				*/
				template <typename T>
				GAIA_NODISCARD auto view_mut() {
					using U = typename component::component_type_t<T>::Type;
					static_assert(!std::is_same_v<U, Entity>);

					return mem::auto_view_policy_set<U>{view_mut_inter<T, true>()};
				}

				/*!
				Returns a mutable component view.
				Doesn't update the world version when the access is aquired.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\return Component view with read-write access
				*/
				template <typename T>
				GAIA_NODISCARD auto sview_mut() {
					using U = typename component::component_type_t<T>::Type;
					static_assert(!std::is_same_v<U, Entity>);

					return mem::auto_view_policy_set<U>{view_mut_inter<T, false>()};
				}

				/*!
				Make \param entity a part of the chunk at the version of the world
				\return Index of the entity within the chunk.
				*/
				GAIA_NODISCARD uint32_t add_entity(Entity entity) {
					const auto index = m_header.count++;
					++m_header.countEnabled;
					set_entity(index, entity);

					update_version(m_header.worldVersion);
					update_world_version(component::ComponentType::CT_Generic);
					update_world_version(component::ComponentType::CT_Chunk);

					return index;
				}

				/*!
				Copies all data associated with \param oldEntity into \param newEntity.
				*/
				static void copy_entity_data(Entity oldEntity, Entity newEntity, std::span<EntityContainer> entities) {
					GAIA_PROF_SCOPE(copy_entity_data);

					auto& oldEntityContainer = entities[oldEntity.id()];
					auto* pOldChunk = oldEntityContainer.pChunk;

					auto& newEntityContainer = entities[newEntity.id()];
					auto* pNewChunk = newEntityContainer.pChunk;

					GAIA_ASSERT(pOldChunk->archetype_id() == pNewChunk->archetype_id());

					const auto& cc = ComponentCache::get();
					auto oldIds = pOldChunk->comp_id_view(component::ComponentType::CT_Generic);
					auto oldOffs = pOldChunk->comp_offset_view(component::ComponentType::CT_Generic);

					// Copy generic component data from reference entity to our new entity
					for (uint32_t i = 0; i < oldIds.size(); ++i) {
						const auto& desc = cc.comp_desc(oldIds[i]);
						if (desc.properties.size == 0U)
							continue;

						const auto offset = oldOffs[i];
						const auto idxSrc = offset + desc.properties.size * (uint32_t)oldEntityContainer.idx;
						const auto idxDst = offset + desc.properties.size * (uint32_t)newEntityContainer.idx;

						GAIA_ASSERT(idxSrc < pOldChunk->bytes());
						GAIA_ASSERT(idxDst < pNewChunk->bytes());

						auto* pSrc = (void*)&pOldChunk->data(idxSrc);
						auto* pDst = (void*)&pNewChunk->data(idxDst);
						desc.copy(pSrc, pDst);
					}
				}

				/*!
				Moves all data associated with \param entity into the chunk so that it is stored at \param newEntityIdx.
				*/
				void move_entity_data(Entity entity, uint32_t newEntityIdx, std::span<EntityContainer> entities) {
					GAIA_PROF_SCOPE(CopyEntityFrom);

					auto& oldEntityContainer = entities[entity.id()];
					auto* pOldChunk = oldEntityContainer.pChunk;

					GAIA_ASSERT(pOldChunk->archetype_id() == archetype_id());

					const auto& cc = ComponentCache::get();
					auto oldIds = pOldChunk->comp_id_view(component::ComponentType::CT_Generic);
					auto oldOffs = pOldChunk->comp_offset_view(component::ComponentType::CT_Generic);

					// Copy generic component data from reference entity to our new entity
					for (uint32_t i = 0; i < oldIds.size(); ++i) {
						const auto& desc = cc.comp_desc(oldIds[i]);
						if (desc.properties.size == 0U)
							continue;

						const auto offset = oldOffs[i];
						const auto idxSrc = offset + desc.properties.size * (uint32_t)oldEntityContainer.idx;
						const auto idxDst = offset + desc.properties.size * newEntityIdx;

						GAIA_ASSERT(idxSrc < pOldChunk->bytes());
						GAIA_ASSERT(idxDst < bytes());

						auto* pSrc = (void*)&pOldChunk->data(idxSrc);
						auto* pDst = (void*)&data(idxDst);
						desc.ctor_from(pSrc, pDst);
					}
				}

				/*!
				Moves all data associated with \param entity into the chunk so that it is stored at index \param newEntityIdx.
				*/
				void move_foreign_entity_data(Entity entity, uint32_t newEntityIdx, std::span<EntityContainer> entities) {
					GAIA_PROF_SCOPE(move_foreign_entity_data);

					auto& oldEntityContainer = entities[entity.id()];
					auto* pOldChunk = oldEntityContainer.pChunk;

					const auto& cc = ComponentCache::get();

					// Find intersection of the two component lists.
					// We ignore chunk components here because they should't be influenced
					// by entities moving around.
					auto oldIds = pOldChunk->comp_id_view(component::ComponentType::CT_Generic);
					auto oldOffs = pOldChunk->comp_offset_view(component::ComponentType::CT_Generic);
					auto newIds = comp_id_view(component::ComponentType::CT_Generic);
					auto newOffs = comp_offset_view(component::ComponentType::CT_Generic);

					// Arrays are sorted so we can do linear intersection lookup
					{
						uint32_t i = 0;
						uint32_t j = 0;

						auto moveData = [&](const component::ComponentDesc& desc) {
							if (desc.properties.size == 0U)
								return;

							// Let's move all type data from oldEntity to newEntity
							const auto idxSrc = oldOffs[i] + desc.properties.size * (uint32_t)oldEntityContainer.idx;
							const auto idxDst = newOffs[j] + desc.properties.size * newEntityIdx;

							GAIA_ASSERT(idxSrc < pOldChunk->bytes());
							GAIA_ASSERT(idxDst < bytes());

							auto* pSrc = (void*)&pOldChunk->data(idxSrc);
							auto* pDst = (void*)&data(idxDst);
							desc.ctor_from(pSrc, pDst);
						};

						while (i < oldIds.size() && j < newIds.size()) {
							const auto& descOld = cc.comp_desc(oldIds[i]);
							const auto& descNew = cc.comp_desc(newIds[j]);

							if (&descOld == &descNew) {
								moveData(descOld);
								++i;
								++j;
							} else if (component::SortComponentCond{}.operator()(descOld.componentId, descNew.componentId))
								++i;
							else
								++j;
						}
					}
				}

				/*!
				Tries to remove the entity at index \param index.
				Removal is done via swapping with last entity in chunk.
				Upon removal, all associated data is also removed.
				If the entity at the given index already is the last chunk entity, it is removed directly.
				*/
				void remove_chunk_entity(uint32_t index, std::span<EntityContainer> entities) {
					GAIA_PROF_SCOPE(remove_chunk_entity);

					const auto left = index;
					const auto right = (uint32_t)m_header.count - 1;
					// The "left" entity is the one we are going to destroy so it needs to preceed the "right"
					GAIA_ASSERT(left <= right);

					const auto& cc = ComponentCache::get();

					// There must be at least 2 entities inside to swap
					if GAIA_LIKELY (left < right) {
						GAIA_ASSERT(m_header.count > 1);

						// Update entity index inside chunk
						const auto entity = get_entity(right);
						set_entity(left, entity);

						auto compIds = comp_id_view(component::ComponentType::CT_Generic);
						auto compOffs = comp_offset_view(component::ComponentType::CT_Generic);

						for (uint32_t i = 0; i < compIds.size(); ++i) {
							const auto& desc = cc.comp_desc(compIds[i]);
							if (desc.properties.size == 0U)
								continue;

							const auto offset = compOffs[i];
							const auto idxSrc = offset + left * desc.properties.size;
							const auto idxDst = offset + right * desc.properties.size;

							GAIA_ASSERT(idxSrc < bytes());
							GAIA_ASSERT(idxDst < bytes());
							GAIA_ASSERT(idxSrc != idxDst);

							auto* pSrc = (void*)&m_data[idxSrc];
							auto* pDst = (void*)&m_data[idxDst];
							desc.move(pSrc, pDst);
							desc.dtor(pSrc);
						}

						// Entity has been replaced with the last one in our chunk.
						// Update its index and generation so look ups can find it.
						auto& entityContainer = entities[entity.id()];
						entityContainer.idx = left;
						entityContainer.gen = entity.gen();
					} else {
						auto compIds = comp_id_view(component::ComponentType::CT_Generic);
						auto compOffs = comp_offset_view(component::ComponentType::CT_Generic);

						for (uint32_t i = 0; i < compIds.size(); ++i) {
							const auto& desc = cc.comp_desc(compIds[i]);
							if (desc.properties.size == 0U)
								continue;

							const auto offset = compOffs[i];
							const auto idxSrc = offset + left * desc.properties.size;

							GAIA_ASSERT(idxSrc < bytes());

							auto* pSrc = (void*)&m_data[idxSrc];
							desc.dtor(pSrc);
						}
					}
				}

				/*!
				Tries to swap the entity at index \param left with the one at the index \param right.
				When swapping, all data associated with the two entities is swapped as well.
				If \param left equals \param right no swapping is performed.
				\warning "Left" must he smaller or equal to "right"
				*/
				void swap_chunk_entities(uint32_t left, uint32_t right, std::span<EntityContainer> entities) {
					// The "left" entity is the one we are going to destroy so it needs to preceed the "right".
					// Unlike remove_chunk_entity, it is not technically necessary but we do it
					// anyway for the sake of consistency.
					GAIA_ASSERT(left <= right);

					// If there are at least two entities inside to swap
					if GAIA_UNLIKELY (m_header.count <= 1)
						return;
					if (left == right)
						return;

					GAIA_PROF_SCOPE(SwapEntitiesInsideChunk);

					// Update entity indices inside chunk
					const auto entityLeft = get_entity(left);
					const auto entityRight = get_entity(right);
					set_entity(left, entityRight);
					set_entity(right, entityLeft);

					const auto& cc = ComponentCache::get();
					auto compIds = comp_id_view(component::ComponentType::CT_Generic);
					auto compOffs = comp_offset_view(component::ComponentType::CT_Generic);

					for (uint32_t i = 0; i < compIds.size(); ++i) {
						const auto& desc = cc.comp_desc(compIds[i]);
						if (desc.properties.size == 0U)
							continue;

						const auto offset = compOffs[i];
						const auto idxSrc = offset + left * desc.properties.size;
						const auto idxDst = offset + right * desc.properties.size;

						GAIA_ASSERT(idxSrc < bytes());
						GAIA_ASSERT(idxDst < bytes());
						GAIA_ASSERT(idxSrc != idxDst);

						auto* pSrc = (void*)&m_data[idxSrc];
						auto* pDst = (void*)&m_data[idxDst];
						desc.swap(pSrc, pDst);
					}

					// Entities were swapped. Update their entity container records.
					auto& ecLeft = entities[entityLeft.id()];
					bool ecLeftWasDisabled = ecLeft.dis;
					auto& ecRight = entities[entityRight.id()];
					ecLeft.idx = right;
					ecLeft.gen = entityRight.gen();
					ecLeft.dis = ecRight.dis;
					ecRight.idx = left;
					ecRight.gen = entityLeft.gen();
					ecRight.dis = ecLeftWasDisabled;
				}

				/*!
				Makes the entity a part of a chunk on a given index.
				\param index Index of the entity
				\param entity Entity to store in the chunk
				*/
				void set_entity(uint32_t index, Entity entity) {
					GAIA_ASSERT(index < m_header.count && "Entity chunk index out of bounds!");

					const auto offset = sizeof(Entity) * index + m_header.offsets.firstByte_EntityData;
					// unaligned_ref not necessary because data is aligned
					auto* pMem = (Entity*)&m_data[offset];
					*pMem = entity;
				}

				/*!
				Returns the entity on a given index in the chunk.
				\param index Index of the entity
				\return Entity on a given index within the chunk.
				*/
				GAIA_NODISCARD Entity get_entity(uint32_t index) const {
					GAIA_ASSERT(index < m_header.count && "Entity chunk index out of bounds!");

					const auto offset = sizeof(Entity) * index + m_header.offsets.firstByte_EntityData;
					// unaligned_ref not necessary because data is aligned
					auto* pMem = (Entity*)&m_data[offset];
					return *pMem;
				}

				/*!
				Enables or disables the entity on a given index in the chunk.
				\param index Index of the entity
				\param enableEntity Enables or disabled the entity
				*/
				void enable_entity(uint32_t index, bool enableEntity, std::span<EntityContainer> entities) {
					GAIA_ASSERT(index < m_header.count && "Entity chunk index out of bounds!");

					if (enableEntity) {
						// Nothing to enable if there are no disabled entities
						if (!m_header.has_disabled_entities())
							return;
						// Trying to enable an already enabled entity
						if (enabled(index))
							return;
						// Try swapping our entity with the last disabled one
						swap_chunk_entities(--m_header.firstEnabledEntityIndex, index, entities);
						entities[get_entity(index).id()].dis = 0;
						++m_header.countEnabled;
					} else {
						// Nothing to disable if there are no enabled entities
						if (!m_header.has_enabled_entities())
							return;
						// Trying to disable an already disabled entity
						if (!enabled(index))
							return;
						// Try swapping our entity with the last one in our chunk
						swap_chunk_entities(m_header.firstEnabledEntityIndex++, index, entities);
						entities[get_entity(index).id()].dis = 1;
						--m_header.countEnabled;
					}
				}

				bool enabled(uint32_t index) const {
					GAIA_ASSERT(m_header.count > 0);

					return index >= (uint32_t)m_header.firstEnabledEntityIndex;
				}

				/*!
				Returns a pointer to chunk data.
				\param offset Offset into chunk data
				\return Pointer to chunk data.
				*/
				uint8_t& data(uint32_t offset) {
					return m_data[offset];
				}

				/*!
				Returns a pointer to chunk data.
				\param offset Offset into chunk data
				\return Pointer to chunk data.
				*/
				const uint8_t& data(uint32_t offset) const {
					return m_data[offset];
				}

				/*!
				Returns an offset to chunk data at which the component is stored.
				\warning It is expected the component with \param componentId is present. Undefined behavior otherwise.
				\param compType Component type
				\param componentId Component id
				\param componentIdx Index of the component in this chunk's component array
				\return Offset from the start of chunk data area.
				*/
				GAIA_NODISCARD ChunkComponentOffset find_data_offset(
						component::ComponentType compType, component::ComponentId componentId, uint32_t& componentIdx) const {
					// Don't use this with empty components. It's impossible to write to them anyway.
					GAIA_ASSERT(ComponentCache::get().comp_desc(componentId).properties.size != 0);

					componentIdx = comp_idx(compType, componentId);

					auto compOffs = comp_offset_view(compType);
					const auto offset = compOffs[componentIdx];
					GAIA_ASSERT(offset >= m_header.offsets.firstByte_EntityData);
					return offset;
				}

				/*!
				Returns an offset to chunk data at which the component is stored.
				\warning It is expected the component with \param componentId is present. Undefined behavior otherwise.
				\param compType Component type
				\param componentId Component id
				\return Offset from the start of chunk data area.
				*/
				GAIA_NODISCARD GAIA_FORCEINLINE ChunkComponentOffset
				find_data_offset(component::ComponentType compType, component::ComponentId componentId) const {
					[[maybe_unused]] uint32_t componentIdx = 0;
					return find_data_offset(compType, componentId, componentIdx);
				}

				//----------------------------------------------------------------------
				// Component handling
				//----------------------------------------------------------------------

				bool has_custom_generic_ctor() const {
					return m_header.hasAnyCustomGenericCtor;
				}

				bool has_custom_chunk_ctor() const {
					return m_header.hasAnyCustomChunkCtor;
				}

				bool has_custom_generic_dtor() const {
					return m_header.hasAnyCustomGenericDtor;
				}

				bool has_custom_chunk_dtor() const {
					return m_header.hasAnyCustomChunkDtor;
				}

				void call_ctor(component::ComponentType compType, component::ComponentId componentId, uint32_t entIdx) {
					GAIA_PROF_SCOPE(call_ctor);

					// Make sure only generic types are used with indices
					GAIA_ASSERT(compType == component::ComponentType::CT_Generic || entIdx == 0);

					const auto& cc = ComponentCache::get();
					const auto& desc = cc.comp_desc(componentId);
					if (desc.func_ctor == nullptr)
						return;

					const auto idx = comp_idx(compType, componentId);

					auto compOffs = comp_offset_view(compType);
					const auto offset = compOffs[idx];
					const auto idxSrc = offset + entIdx * desc.properties.size;
					GAIA_ASSERT(idxSrc < bytes());

					auto* pSrc = (void*)&m_data[idxSrc];
					desc.func_ctor(pSrc, 1);
				}

				void call_ctors(component::ComponentType compType, uint32_t entIdx, uint32_t entCnt) {
					GAIA_PROF_SCOPE(call_ctors);

					GAIA_ASSERT(
							compType == component::ComponentType::CT_Generic && has_custom_generic_ctor() ||
							compType == component::ComponentType::CT_Chunk && has_custom_chunk_ctor());

					// Make sure only generic types are used with indices
					GAIA_ASSERT(compType == component::ComponentType::CT_Generic || (entIdx == 0 && entCnt == 1));

					const auto& cc = ComponentCache::get();
					auto compIds = comp_id_view(compType);
					auto compOffs = comp_offset_view(compType);

					for (uint32_t i = 0; i < compIds.size(); ++i) {
						const auto& desc = cc.comp_desc(compIds[i]);
						if (desc.func_ctor == nullptr)
							continue;

						const auto offset = compOffs[i];
						const auto idxSrc = offset + entIdx * desc.properties.size;
						GAIA_ASSERT(idxSrc < bytes());

						auto* pSrc = (void*)&m_data[idxSrc];
						desc.func_ctor(pSrc, entCnt);
					}
				}

				void call_dtors(component::ComponentType compType, uint32_t entIdx, uint32_t entCnt) {
					GAIA_PROF_SCOPE(call_dtors);

					GAIA_ASSERT(
							compType == component::ComponentType::CT_Generic && has_custom_generic_dtor() ||
							compType == component::ComponentType::CT_Chunk && has_custom_chunk_dtor());

					// Make sure only generic types are used with indices
					GAIA_ASSERT(compType == component::ComponentType::CT_Generic || (entIdx == 0 && entCnt == 1));

					const auto& cc = ComponentCache::get();
					auto compIds = comp_id_view(compType);
					auto compOffs = comp_offset_view(compType);

					for (uint32_t i = 0; i < compIds.size(); ++i) {
						const auto& desc = cc.comp_desc(compIds[i]);
						if (desc.func_dtor == nullptr)
							continue;

						const auto offset = compOffs[i];
						const auto idxSrc = offset + entIdx * desc.properties.size;
						GAIA_ASSERT(idxSrc < bytes());

						auto* pSrc = (void*)&m_data[idxSrc];
						desc.func_dtor(pSrc, entCnt);
					}
				};

				//----------------------------------------------------------------------
				// Check component presence
				//----------------------------------------------------------------------

				/*!
				Checks if a component with \param componentId and type \param compType is present in the chunk.
				\param componentId Component id
				\param compType Component type
				\return True if found. False otherwise.
				*/
				GAIA_NODISCARD bool has(component::ComponentType compType, component::ComponentId componentId) const {
					auto compIds = comp_id_view(compType);
					return core::has(compIds, componentId);
				}

				/*!
				Checks if component \tparam T is present in the chunk.
				\tparam T Component
				\return True if the component is present. False otherwise.
				*/
				template <typename T>
				GAIA_NODISCARD bool has() const {
					const auto componentId = component::comp_id<T>();

					constexpr auto compType = component::component_type_v<T>;
					return has(compType, componentId);
				}

				//----------------------------------------------------------------------
				// Set component data
				//----------------------------------------------------------------------

				/*!
				Sets the value of the chunk component \tparam T on \param index in the chunk.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\param index Index of entity in the chunk
				\param value Value to set for the component
				*/
				template <typename T, typename U = typename component::component_type_t<T>::Type>
				U& set(uint32_t index) {
					static_assert(
							component::component_type_v<T> == component::ComponentType::CT_Generic,
							"Set providing an index can only be used with generic components");

					// Update the world version
					update_version(m_header.worldVersion);

					GAIA_ASSERT(index < m_header.capacity);
					return view_mut<T>()[index];
				}

				/*!
				Sets the value of the chunk component \tparam T on \param index in the chunk.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\param index Index of entity in the chunk
				\param value Value to set for the component
				*/
				template <typename T, typename U = typename component::component_type_t<T>::Type>
				U& set() {
					// Update the world version
					update_version(m_header.worldVersion);

					GAIA_ASSERT(0 < m_header.capacity);
					return view_mut<T>()[0];
				}

				/*!
				Sets the value of the chunk component \tparam T on \param index in the chunk.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\param index Index of entity in the chunk
				\param value Value to set for the component
				*/
				template <typename T, typename U = typename component::component_type_t<T>::Type>
				void set(uint32_t index, U&& value) {
					static_assert(
							component::component_type_v<T> == component::ComponentType::CT_Generic,
							"Set providing an index can only be used with generic components");

					// Update the world version
					update_version(m_header.worldVersion);

					GAIA_ASSERT(index < m_header.capacity);
					view_mut<T>()[index] = std::forward<U>(value);
				}

				/*!
				Sets the value of the chunk component \tparam T in the chunk.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\param value Value to set for the component
				*/
				template <typename T, typename U = typename component::component_type_t<T>::Type>
				void set(U&& value) {
					static_assert(
							component::component_type_v<T> != component::ComponentType::CT_Generic,
							"Set not providing an index can only be used with non-generic components");

					// Update the world version
					update_version(m_header.worldVersion);

					GAIA_ASSERT(0 < m_header.capacity);
					view_mut<T>()[0] = std::forward<U>(value);
				}

				/*!
				Sets the value of the chunk component \tparam T on \param index in the chunk.
				\warning World version is not updated so Query filters will not be able to catch this change.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\param index Index of entity in the chunk
				\param value Value to set for the component
				*/
				template <typename T, typename U = typename component::component_type_t<T>::Type>
				void sset(uint32_t index, U&& value) {
					static_assert(
							component::component_type_v<T> == component::ComponentType::CT_Generic,
							"SetSilent providing an index can only be used with generic components");

					GAIA_ASSERT(index < m_header.capacity);
					sview_mut<T>()[index] = std::forward<U>(value);
				}

				/*!
				Sets the value of the chunk component \tparam T in the chunk.
				\warning World version is not updated so Query filters will not be able to catch this change.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\param value Value to set for the component
				*/
				template <typename T, typename U = typename component::component_type_t<T>::Type>
				void sset(U&& value) {
					static_assert(
							component::component_type_v<T> != component::ComponentType::CT_Generic,
							"SetSilent not providing an index can only be used with non-generic components");

					GAIA_ASSERT(0 < m_header.capacity);
					sview_mut<T>()[0] = std::forward<U>(value);
				}

				//----------------------------------------------------------------------
				// Read component data
				//----------------------------------------------------------------------

				/*!
				Returns the value stored in the component \tparam T on \param index in the chunk.
				\warning It is expected the \param index is valid. Undefined behavior otherwise.
				\warning It is expected the component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\param index Index of entity in the chunk
				\return Value stored in the component.
				*/
				template <typename T>
				GAIA_NODISCARD auto get(uint32_t index) const {
					static_assert(
							component::component_type_v<T> == component::ComponentType::CT_Generic,
							"Get providing an index can only be used with generic components");

					return comp_inter<T>(index);
				}

				/*!
				Returns the value stored in the chunk component \tparam T.
				\warning It is expected the chunk component \tparam T is present. Undefined behavior otherwise.
				\tparam T Component
				\return Value stored in the component.
				*/
				template <typename T>
				GAIA_NODISCARD auto get() const {
					static_assert(
							component::component_type_v<T> != component::ComponentType::CT_Generic,
							"Get not providing an index can only be used with non-generic components");

					return comp_inter<T>(0);
				}

				/*!
				Returns the internal index of a component based on the provided \param componentId.
				\param compType Component type
				\return Component index if the component was found. -1 otherwise.
				*/
				GAIA_NODISCARD uint32_t comp_idx(component::ComponentType compType, component::ComponentId componentId) const {
					auto compIds = comp_id_view(compType);
					const auto idx = core::get_index_unsafe(compIds, componentId);
					GAIA_ASSERT(idx != BadIndex);
					return idx;
				}

				//----------------------------------------------------------------------
				// Iteration
				//----------------------------------------------------------------------

				template <typename T>
				GAIA_NODISCARD constexpr GAIA_FORCEINLINE auto comp_view() {
					using U = typename component::component_type_t<T>::Type;
					using UOriginal = typename component::component_type_t<T>::TypeOriginal;
					if constexpr (component::is_component_mut_v<UOriginal>) {
						auto s = view_mut_inter<U, true>();
						return std::span{(U*)s.data(), s.size()};
					} else {
						auto s = view_inter<U>();
						return std::span{(const U*)s.data(), s.size()};
					}
				}

				template <typename... T, typename Func>
				GAIA_FORCEINLINE void each([[maybe_unused]] core::func_type_list<T...> types, Func func) {
					const uint32_t idxFrom = m_header.firstEnabledEntityIndex;
					const uint32_t idxStop = m_header.count;
					GAIA_ASSERT(idxStop > idxFrom);
					GAIA_ASSERT(idxStop > 0);

					if constexpr (sizeof...(T) > 0) {
						// Pointers to the respective component types in the chunk, e.g
						// 		q.each([&](Position& p, const Velocity& v) {...}
						// Translates to:
						//  	auto p = iter.view_mut_inter<Position, true>();
						//		auto v = iter.view_inter<Velocity>();
						auto dataPointerTuple = std::make_tuple(comp_view<T>()...);

						// Iterate over each entity in the chunk.
						// Translates to:
						//		for (uint32_t i: iter)
						//			func(p[i], v[i]);

						for (uint32_t i = idxFrom; i < idxStop; ++i)
							func(std::get<decltype(comp_view<T>())>(dataPointerTuple)[i]...);
					} else {
						// No functor parameters. Do an empty loop.
						for (uint32_t i = idxFrom; i < idxStop; ++i)
							func();
					}
				}

				//----------------------------------------------------------------------

				GAIA_NODISCARD ArchetypeId archetype_id() const {
					return m_header.archetypeId;
				}

				//! Sets the index of this chunk in its archetype's storage
				void set_idx(uint32_t value) {
					m_header.index = value;
				}

				//! Returns the index of this chunk in its archetype's storage
				GAIA_NODISCARD uint32_t idx() const {
					return m_header.index;
				}

				//! Checks is this chunk has any enabled entities
				GAIA_NODISCARD bool has_enabled_entities() const {
					return m_header.has_enabled_entities();
				}

				//! Checks is this chunk has any disabled entities
				GAIA_NODISCARD bool has_disabled_entities() const {
					return m_header.has_disabled_entities();
				}

				//! Checks is this chunk is dying
				GAIA_NODISCARD bool dying() const {
					return m_header.lifespanCountdown > 0;
				}

				void prepare_to_die() {
					m_header.lifespanCountdown = ChunkHeader::MAX_CHUNK_LIFESPAN;
				}

				bool progress_death() {
					GAIA_ASSERT(dying());
					--m_header.lifespanCountdown;
					return dying();
				}

				void set_structural_changes(bool value) {
					if (value) {
						GAIA_ASSERT(m_header.structuralChangesLocked < 16);
						++m_header.structuralChangesLocked;
					} else {
						GAIA_ASSERT(m_header.structuralChangesLocked > 0);
						--m_header.structuralChangesLocked;
					}
				}

				bool has_structural_changes() const {
					return m_header.structuralChangesLocked != 0;
				}

				//! Checks is the full capacity of the has has been reached
				GAIA_NODISCARD bool full() const {
					return m_header.count >= m_header.capacity;
				}

				//! Checks is the chunk is semi-full.
				GAIA_NODISCARD bool is_semi() const {
					constexpr float Threshold = 0.7f;
					return ((float)m_header.count / (float)m_header.capacity) < Threshold;
				}

				//! Checks is there are any entities in the chunk
				GAIA_NODISCARD bool has_entities() const {
					return m_header.count > 0;
				}

				//! Returns the total number of entities in the chunk (both enabled and disabled)
				GAIA_NODISCARD uint32_t size() const {
					return m_header.count;
				}

				//! Return the number of entities in the chunk which are enabled
				GAIA_NODISCARD uint32_t size_enabled() const {
					return m_header.countEnabled;
				}

				//! Return the number of entities in the chunk which are enabled
				GAIA_NODISCARD uint32_t size_disabled() const {
					return m_header.firstEnabledEntityIndex;
				}

				//! Returns the number of entities in the chunk
				GAIA_NODISCARD uint32_t capacity() const {
					return m_header.capacity;
				}

				GAIA_NODISCARD uint32_t bytes() const {
					return detail::ChunkAllocatorImpl::mem_block_size(m_header.sizeType);
				}

				GAIA_NODISCARD std::span<uint32_t> comp_version_view(component::ComponentType compType) const {
					const auto offset = m_header.offsets.firstByte_Versions[compType];
					return {(uint32_t*)(&m_data[offset]), m_header.componentCount[compType]};
				}

				GAIA_NODISCARD std::span<const component::ComponentId> comp_id_view(component::ComponentType compType) const {
					using RetType = std::add_pointer_t<const component::ComponentId>;
					const auto offset = m_header.offsets.firstByte_ComponentIds[compType];
					return {(RetType)&m_data[offset], m_header.componentCount[compType]};
				}

				GAIA_NODISCARD std::span<const ChunkComponentOffset> comp_offset_view(component::ComponentType compType) const {
					using RetType = std::add_pointer_t<const ChunkComponentOffset>;
					const auto offset = m_header.offsets.firstByte_ComponentOffsets[compType];
					return {(RetType)&m_data[offset], m_header.componentCount[compType]};
				}

				//! Returns true if the provided version is newer than the one stored internally
				GAIA_NODISCARD bool changed(component::ComponentType compType, uint32_t version, uint32_t componentIdx) const {
					auto versions = comp_version_view(compType);
					return version_changed(versions[componentIdx], version);
				}

				//! Update version of a component at the index \param componentIdx of a given \param compType
				GAIA_FORCEINLINE void update_world_version(component::ComponentType compType, uint32_t componentIdx) const {
					// Make sure only proper input is provided
					GAIA_ASSERT(componentIdx >= 0 && componentIdx < archetype::MAX_COMPONENTS_PER_ARCHETYPE);

					auto versions = comp_version_view(compType);
					versions[componentIdx] = m_header.worldVersion;
				}

				//! Update version of all components of a given \param compType
				GAIA_FORCEINLINE void update_world_version(component::ComponentType compType) const {
					auto versions = comp_version_view(compType);
					for (auto& v: versions)
						v = m_header.worldVersion;
				}

				void diag(uint32_t index) const {
					GAIA_LOG_N(
							"  Chunk #%04u, entities:%u/%u, lifespanCountdown:%u", index, m_header.count, m_header.capacity,
							m_header.lifespanCountdown);
				}
			};
		} // namespace archetype
	} // namespace ecs
} // namespace gaia
