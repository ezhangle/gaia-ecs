#pragma once

#include <cstdint>

#include "../cnt/map.h"
#include "../config/logging.h"
#include "archetype_common.h"
#include "component.h"
#include "id.h"

namespace gaia {
	namespace ecs {
		class World;
		const char* entity_name(const World& world, Entity entity);
		const char* entity_name(const World& world, EntityId entityId);

		using ArchetypeGraphEdge = ArchetypeIdHashPair;

		class ArchetypeGraph {
			using EdgeMap = cnt::map<EntityLookupKey, ArchetypeGraphEdge>;

			//! Map of edges in the archetype graph when adding components
			EdgeMap m_edgesAdd;
			//! Map of edges in the archetype graph when removing components
			EdgeMap m_edgesDel;

		private:
			void add_edge(EdgeMap& edges, Entity entity, ArchetypeId archetypeId, ArchetypeIdHash hash) {
				[[maybe_unused]] const auto ret =
						edges.try_emplace(EntityLookupKey(entity), ArchetypeGraphEdge{archetypeId, hash});
				GAIA_ASSERT(ret.second);
			}

			void del_edge(EdgeMap& edges, Entity entity) {
				edges.erase(EntityLookupKey(entity));
			}

			GAIA_NODISCARD ArchetypeGraphEdge find_edge(const EdgeMap& edges, Entity entity) const {
				const auto it = edges.find(EntityLookupKey(entity));
				return it != edges.end() ? it->second : ArchetypeIdHashPairBad;
			}

		public:
			//! Creates an "add" edge in the graph leading to the target archetype.
			//! \param entity Edge entity.
			//! \param archetypeId Target archetype.
			void add_edge_right(Entity entity, ArchetypeId archetypeId, ArchetypeIdHash hash) {
				add_edge(m_edgesAdd, entity, archetypeId, hash);
			}

			//! Creates a "del" edge in the graph leading to the target archetype.
			//! \param entity Edge entity.
			//! \param archetypeId Target archetype.
			void add_edge_left(Entity entity, ArchetypeId archetypeId, ArchetypeIdHash hash) {
				add_edge(m_edgesDel, entity, archetypeId, hash);
			}

			//! Deletes the "add" edge formed by the entity \param entity.
			void del_edge_right(Entity entity) {
				del_edge(m_edgesAdd, entity);
			}

			//! Deletes the "del" edge formed by the entity \param entity.
			void del_edge_left(Entity entity) {
				del_edge(m_edgesDel, entity);
			}

			//! Checks if an archetype graph "add" edge with entity \param entity exists.
			//! \return Archetype id of the target archetype if the edge is found. ArchetypeGraphEdgeBad otherwise.
			GAIA_NODISCARD ArchetypeGraphEdge find_edge_right(Entity entity) const {
				return find_edge(m_edgesAdd, entity);
			}

			//! Checks if an archetype graph "del" edge with entity \param entity exists.
			//! \return Archetype id of the target archetype if the edge is found. ArchetypeGraphEdgeBad otherwise.
			GAIA_NODISCARD ArchetypeGraphEdge find_edge_left(Entity entity) const {
				return find_edge(m_edgesDel, entity);
			}

			void diag(const World& world) const {
				auto diagEdge = [&](const auto& edges) {
					for (const auto& edge: edges) {
						const auto entity = edge.first.entity();
						if (entity.pair()) {
							const auto* name0 = entity_name(world, entity.id());
							const auto* name1 = entity_name(world, entity.gen());
							GAIA_LOG_N(
									"      pair [%u:%u], %s -> %s, aid:%u",
									//
									entity.id(), entity.gen(), name0, name1, edge.second.id);
						} else {
							const auto* name = entity_name(world, entity);
							GAIA_LOG_N(
									"      ent [%u:%u], %s [%s], aid:%u",
									//
									entity.id(), entity.gen(), name, EntityKindString[entity.kind()], edge.second.id);
						}
					}
				};

				// Add edges (movement towards the leafs)
				if (!m_edgesAdd.empty()) {
					GAIA_LOG_N("  Add edges - count:%u", (uint32_t)m_edgesAdd.size());
					diagEdge(m_edgesAdd);
				}

				// Delete edges (movement towards the root)
				if (!m_edgesDel.empty()) {
					GAIA_LOG_N("  Del edges - count:%u", (uint32_t)m_edgesDel.size());
					diagEdge(m_edgesDel);
				}
			}
		};
	} // namespace ecs
} // namespace gaia