#ifndef OSMIUM_AREA_ASSEMBLER_HPP
#define OSMIUM_AREA_ASSEMBLER_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2014 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <algorithm>
#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <vector>

#include <osmium/memory/buffer.hpp>
#include <osmium/osm/area.hpp>
#include <osmium/osm/builder.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/ostream.hpp>
#include <osmium/osm/relation.hpp>
#include <osmium/tags/key_filter.hpp>

#include <osmium/area/detail/proto_ring.hpp>
#include <osmium/area/detail/node_ref_segment.hpp>
#include <osmium/area/detail/segment_list.hpp>
#include <osmium/area/problem_reporter.hpp>

namespace osmium {

    namespace area {

        using osmium::area::detail::ProtoRing;

        struct AssemblerConfig {

            osmium::area::ProblemReporter* problem_reporter;

            // Enables debug output to stderr
            bool debug;

            AssemblerConfig(osmium::area::ProblemReporter* pr = nullptr, bool d=false) :
                problem_reporter(pr),
                debug(d) {
            }

            ~AssemblerConfig() = default;

            /**
             * Enable or disable debug output to stderr. This is for Osmium
             * developers only.
             */
            void enable_debug_output(bool d=true) {
                debug = d;
            }

        }; // struct AssemblerConfig

        /**
         * Assembles area objects from multipolygon relations and their
         * members. This is called by the MultipolygonCollector object
         * after all members have been collected.
         */
        class Assembler {

            const AssemblerConfig m_config;

            // The way segments
            osmium::area::detail::SegmentList m_segment_list;

            // The rings we are building from the way segments
            std::list<ProtoRing> m_rings {};

            std::vector<ProtoRing*> m_outer_rings {};
            std::vector<ProtoRing*> m_inner_rings {};

            int m_inner_outer_mismatches { 0 };

            bool debug() const {
                return m_config.debug;
            }

            /**
             * Checks whether the given NodeRefs have the same location.
             * Uses the actual location for the test, not the id. If both
             * have the same location, but not the same id, a problem
             * point will be added to the list of problem points.
             */
            bool has_same_location(const osmium::NodeRef& nr1, const osmium::NodeRef& nr2) {
                if (nr1.location() != nr2.location()) {
                    return false;
                }
                if (nr1.ref() != nr2.ref()) {
                    if (m_config.problem_reporter) {
                        m_config.problem_reporter->report_duplicate_node(nr1.ref(), nr2.ref(), nr1.location());
                    }
                }
                return true;
            }

            /**
             * Initialize area attributes and tags from the attributes and tags
             * of the given object.
             */
            void initialize_area_from_object(osmium::osm::AreaBuilder& builder, const osmium::Object& object, int id_offset) const {
                osmium::Area& area = builder.object();
                area.id(object.id() * 2 + id_offset);
                area.version(object.version());
                area.changeset(object.changeset());
                area.timestamp(object.timestamp());
                area.visible(object.visible());
                area.uid(object.uid());

                builder.add_user(object.user());
            }

            void add_tags_to_area(osmium::osm::AreaBuilder& builder, const osmium::Way& way) const {
                osmium::osm::TagListBuilder tl_builder(builder.buffer(), &builder);
                for (const osmium::Tag& tag : way.tags()) {
                    tl_builder.add_tag(tag.key(), tag.value());
                }
            }

            void add_common_tags(osmium::osm::TagListBuilder& tl_builder, std::set<const osmium::Way*>& ways) const {
                std::map<std::string, size_t> counter;
                for (const osmium::Way* way : ways) {
                    for (auto& tag : way->tags()) {
                        std::string kv {tag.key()};
                        kv.append(1, '\0');
                        kv.append(tag.value());
                        ++counter[kv];
                    }
                }

                size_t num_ways = ways.size();
                for (auto& t_c : counter) {
                    if (debug()) {
                        std::cerr << "        tag " << t_c.first << " is used " << t_c.second << " times in " << num_ways << " ways\n";
                    }
                    if (t_c.second == num_ways) {
                        size_t len = std::strlen(t_c.first.c_str());
                        tl_builder.add_tag(t_c.first.c_str(), t_c.first.c_str() + len + 1);
                    }
                }
            }

            void add_tags_to_area(osmium::osm::AreaBuilder& builder, const osmium::Relation& relation) const {
                osmium::tags::KeyFilter filter(true);
                filter.add(false, "type").add(false, "created_by").add(false, "source").add(false, "note");
                filter.add(false, "test:id").add(false, "test:section");

                osmium::tags::KeyFilter::iterator fi_begin(filter, relation.tags().begin(), relation.tags().end());
                osmium::tags::KeyFilter::iterator fi_end(filter, relation.tags().end(), relation.tags().end());

                size_t count = std::distance(fi_begin, fi_end);

                if (debug()) {
                    std::cerr << "  found " << count << " tags on relation (without ignored ones)\n";
                }

                if (count > 0) {
                    if (debug()) {
                        std::cerr << "    use tags from relation\n";
                    }

                    // write out all tags except type=*
                    osmium::osm::TagListBuilder tl_builder(builder.buffer(), &builder);
                    for (const osmium::Tag& tag : relation.tags()) {
                        if (strcmp(tag.key(), "type")) {
                            tl_builder.add_tag(tag.key(), tag.value());
                        }
                    }
                } else {
                    if (debug()) {
                        std::cerr << "    use tags from outer ways\n";
                    }
                    std::set<const osmium::Way*> ways;
                    for (auto& ring : m_outer_rings) {
                        ring->get_ways(ways);
                    }
                    if (ways.size() == 1) {
                        if (debug()) {
                            std::cerr << "      only one outer way\n";
                        }
                        osmium::osm::TagListBuilder tl_builder(builder.buffer(), &builder);
                        for (const osmium::Tag& tag : (*ways.begin())->tags()) {
                            tl_builder.add_tag(tag.key(), tag.value());
                        }
                    } else {
                        if (debug()) {
                            std::cerr << "      multiple outer ways, get common tags\n";
                        }
                        osmium::osm::TagListBuilder tl_builder(builder.buffer(), &builder);
                        add_common_tags(tl_builder, ways);
                    }
                }
            }

            /**
             * Go through all the rings and find rings that are not closed.
             * Problems are reported through the problem reporter.
             *
             * @returns true if any rings were not closed, false otherwise
             */
            bool check_for_open_rings() {
                bool open_rings = false;

                for (auto& ring : m_rings) {
                    if (!ring.closed()) {
                        open_rings = true;
                        if (m_config.problem_reporter) {
                            m_config.problem_reporter->report_ring_not_closed(ring.get_segment_front().first().location(), ring.get_segment_back().second().location());
                        }
                    }
                }

                return open_rings;
            }

            /**
             * Check whether there are any rings that can be combined with the
             * given ring to one larger ring by appending the other ring to
             * the end of this ring.
             * If the rings can be combined they are and the function returns
             * true.
             */
            bool possibly_combine_rings_back(ProtoRing& ring) {
                const osmium::NodeRef& nr = ring.get_segment_back().second();

                if (debug()) {
                    std::cerr << "      possibly_combine_rings_back()\n";
                }
                for (auto it = m_rings.begin(); it != m_rings.end(); ++it) {
                    if (&*it != &ring && !it->closed()) {
                        if (has_same_location(nr, it->get_segment_front().first())) {
                            if (debug()) {
                                std::cerr << "      ring.last=it->first\n";
                            }
                            ring.merge_ring(*it, debug());
                            m_rings.erase(it);
                            return true;
                        }
                        if (has_same_location(nr, it->get_segment_back().second())) {
                            if (debug()) {
                                std::cerr << "      ring.last=it->last\n";
                            }
                            ring.merge_ring_reverse(*it, debug());
                            m_rings.erase(it);
                            return true;
                        }
                    }
                }
                return false;
            }

            /**
             * Check whether there are any rings that can be combined with the
             * given ring to one larger ring by prepending the other ring to
             * the start of this ring.
             * If the rings can be combined they are and the function returns
             * true.
             */
            bool possibly_combine_rings_front(ProtoRing& ring) {
                const osmium::NodeRef& nr = ring.get_segment_front().first();

                if (debug()) {
                    std::cerr << "      possibly_combine_rings_front()\n";
                }
                for (auto it = m_rings.begin(); it != m_rings.end(); ++it) {
                    if (&*it != &ring && !it->closed()) {
                        if (has_same_location(nr, it->get_segment_back().second())) {
                            if (debug()) {
                                std::cerr << "      ring.first=it->last\n";
                            }
                            ring.swap_segments(*it);
                            ring.merge_ring(*it, debug());
                            m_rings.erase(it);
                            return true;
                        }
                        if (has_same_location(nr, it->get_segment_front().first())) {
                            if (debug()) {
                                std::cerr << "      ring.first=it->first\n";
                            }
                            ring.reverse();
                            ring.merge_ring(*it, debug());
                            m_rings.erase(it);
                            return true;
                        }
                    }
                }
                return false;
            }

            void split_off_subring(osmium::area::detail::ProtoRing& ring, osmium::area::detail::ProtoRing::segments_type::iterator it, osmium::area::detail::ProtoRing::segments_type::iterator it_begin, osmium::area::detail::ProtoRing::segments_type::iterator it_end) {
                if (debug()) {
                    std::cerr << "        subring found at: " << *it << "\n";
                }
                ProtoRing new_ring(it_begin, it_end);
                ring.remove_segments(it_begin, it_end);
                if (debug()) {
                    std::cerr << "        split into two rings:\n";
                    std::cerr << "          " << new_ring << "\n";
                    std::cerr << "          " << ring << "\n";
                }
                m_rings.push_back(std::move(new_ring));
            }

            bool has_closed_subring_back(ProtoRing& ring, const NodeRef& nr) {
                if (ring.segments().size() < 3) {
                    return false;
                }
                if (debug()) {
                    std::cerr << "      has_closed_subring_back()\n";
                }
                auto end = ring.segments().end();
                for (auto it = ring.segments().begin() + 1; it != end - 1; ++it) {
                    if (has_same_location(nr, it->first())) {
                        split_off_subring(ring, it, it, end);
                        return true;
                    }
                }
                return false;
            }

            bool has_closed_subring_front(ProtoRing& ring, const NodeRef& nr) {
                if (ring.segments().size() < 3) {
                    return false;
                }
                if (debug()) {
                    std::cerr << "      has_closed_subring_front()\n";
                }
                auto end = ring.segments().end();
                for (auto it = ring.segments().begin() + 1; it != end - 1; ++it) {
                    if (has_same_location(nr, it->second())) {
                        split_off_subring(ring, it, ring.segments().begin(), it+1);
                        return true;
                    }
                }
                return false;
            }

            bool check_for_closed_subring(ProtoRing& ring) {
                if (debug()) {
                    std::cerr << "      check_for_closed_subring()\n";
                }

                osmium::area::detail::ProtoRing::segments_type segments(ring.segments().size());
                std::copy(ring.segments().begin(), ring.segments().end(), segments.begin());
                std::sort(segments.begin(), segments.end());
                auto it = std::adjacent_find(segments.begin(), segments.end(), [this](const osmium::area::detail::NodeRefSegment& s1, const osmium::area::detail::NodeRefSegment& s2) {
                    return has_same_location(s1.first(), s2.first());
                });
                if (it == segments.end()) {
                    return false;
                }
                auto r1 = std::find_first_of(ring.segments().begin(), ring.segments().end(), it, it+1);
                assert(r1 != ring.segments().end());
                auto r2 = std::find_first_of(ring.segments().begin(), ring.segments().end(), it+1, it+2);
                assert(r2 != ring.segments().end());

                if (debug()) {
                    std::cerr << "      found subring in ring " << ring << " at " << it->first() << "\n";
                }

                auto m = std::minmax(r1, r2);

                ProtoRing new_ring(m.first, m.second);
                ring.remove_segments(m.first, m.second);

                if (debug()) {
                    std::cerr << "        split ring1=" << new_ring << "\n";
                    std::cerr << "        split ring2=" << ring << "\n";
                }

                m_rings.emplace_back(new_ring);

                return true;
            }

            void combine_rings_front(const osmium::area::detail::NodeRefSegment& segment, ProtoRing& ring) {
                if (debug()) {
                    std::cerr << " => match at front of ring\n";
                }
                ring.add_segment_front(segment);
                has_closed_subring_front(ring, segment.first());
                if (possibly_combine_rings_front(ring)) {
                    check_for_closed_subring(ring);
                }
            }

            void combine_rings_back(const osmium::area::detail::NodeRefSegment& segment, ProtoRing& ring) {
                if (debug()) {
                    std::cerr << " => match at back of ring\n";
                }
                ring.add_segment_back(segment);
                has_closed_subring_back(ring, segment.second());
                if (possibly_combine_rings_back(ring)) {
                    check_for_closed_subring(ring);
                }
            }

            /**
             * Append each outer ring together with its inner rings to the
             * area in the buffer.
             */
            void add_rings_to_area(osmium::osm::AreaBuilder& builder) const {
                for (const ProtoRing* ring : m_outer_rings) {
                    if (debug()) {
                        std::cerr << "    ring " << *ring << " is outer\n";
                    }
                    {
                        osmium::osm::OuterRingBuilder ring_builder(builder.buffer(), &builder);
                        ring_builder.add_node_ref(ring->get_segment_front().first());
                        for (auto& segment : ring->segments()) {
                            ring_builder.add_node_ref(segment.second());
                        }
                    }
                    for (ProtoRing* inner : ring->inner_rings()) {
                        osmium::osm::InnerRingBuilder ring_builder(builder.buffer(), &builder);
                        ring_builder.add_node_ref(inner->get_segment_front().first());
                        for (auto& segment : inner->segments()) {
                            ring_builder.add_node_ref(segment.second());
                        }
                    }
                }
            }

            bool add_to_existing_ring(osmium::area::detail::NodeRefSegment segment) {
                int n=0;
                for (auto& ring : m_rings) {
                    if (debug()) {
                        std::cerr << "    check against ring " << n << " " << ring;
                    }
                    if (ring.closed()) {
                        if (debug()) {
                            std::cerr << " => ring CLOSED\n";
                        }
                    } else {
                        if (has_same_location(ring.get_segment_back().second(), segment.first())) {
                            combine_rings_back(segment, ring);
                            return true;
                        }
                        if (has_same_location(ring.get_segment_back().second(), segment.second())) {
                            segment.swap_locations();
                            combine_rings_back(segment, ring);
                            return true;
                        }
                        if (has_same_location(ring.get_segment_front().first(), segment.first())) {
                            segment.swap_locations();
                            combine_rings_front(segment, ring);
                            return true;
                        }
                        if (has_same_location(ring.get_segment_front().first(), segment.second())) {
                            combine_rings_front(segment, ring);
                            return true;
                        }
                        if (debug()) {
                            std::cerr << " => no match\n";
                        }
                    }

                    ++n;
                }
                return false;
            }

            void check_inner_outer(ProtoRing& ring) {
                const osmium::NodeRef& min_node = ring.min_node();
                if (debug()) {
                    std::cerr << "    check_inner_outer min_node=" << min_node << "\n";
                }

                int count = 0;
                int above = 0;

                for (auto it = m_segment_list.begin(); it != m_segment_list.end() && it->first().location().x() <= min_node.location().x(); ++it) {
                    if (!ring.contains(*it)) {
                        if (debug()) {
                            std::cerr << "      segments for count: " << *it;
                        }
                        if (it->to_left_of(min_node.location())) {
                            ++count;
                            if (debug()) {
                                std::cerr << " counted\n";
                            }
                        } else {
                            if (debug()) {
                                std::cerr << " not counted\n";
                            }
                        }
                        if (it->first().location() == min_node.location()) {
                            if (it->second().location().y() > min_node.location().y()) {
                                ++above;
                            }
                        }
                        if (it->second().location() == min_node.location()) {
                            if (it->first().location().y() > min_node.location().y()) {
                                ++above;
                            }
                        }
                    }
                }

                if (debug()) {
                    std::cerr << "      count=" << count << " above=" << above << "\n";
                }

                count += above % 2;

                if (count % 2) {
                    ring.set_inner();
                }
            }

            void check_inner_outer_roles() {
                if (debug()) {
                    std::cerr << "    check_inner_outer_roles\n";
                }

                for (auto ringptr : m_outer_rings) {
                    for (auto segment : ringptr->segments()) {
                        if (!segment.role_outer()) {
                            ++m_inner_outer_mismatches;
                            if (debug()) {
                                std::cerr << "      segment " << segment << " from way " << segment.way()->id() << " should have role 'outer'\n";
                            }
                            if (m_config.problem_reporter) {
                                m_config.problem_reporter->report_role_should_be_outer(segment.way()->id(), segment.first().location(), segment.second().location());
                            }
                        }
                    }
                }
                for (auto ringptr : m_inner_rings) {
                    for (auto segment : ringptr->segments()) {
                        if (!segment.role_inner()) {
                            ++m_inner_outer_mismatches;
                            if (debug()) {
                                std::cerr << "      segment " << segment << " from way " << segment.way()->id() << " should have role 'inner'\n";
                            }
                            if (m_config.problem_reporter) {
                                m_config.problem_reporter->report_role_should_be_inner(segment.way()->id(), segment.first().location(), segment.second().location());
                            }
                        }
                    }
                }
            }

            /**
             * Create rings from segments.
             */
            bool create_rings() {
                m_segment_list.sort();
                m_segment_list.erase_duplicate_segments();

                // Now we look for segments crossing each other. If there are
                // any, the multipolygon is invalid.
                // In the future this could be improved by trying to fix those
                // cases.
                if (m_segment_list.find_intersections(m_config.problem_reporter)) {
                    return false;
                }

                // Now iterator over all segments and add them to rings. Each segment
                // is tacked on to either end of an existing ring if possible, or a
                // new ring is started with it.
                for (const auto& segment : m_segment_list) {
                    if (debug()) {
                        std::cerr << "  checking segment " << segment << "\n";
                    }
                    if (!add_to_existing_ring(segment)) {
                        if (debug()) {
                            std::cerr << "    new ring for segment " << segment << "\n";
                        }
                        m_rings.emplace_back(segment);
                    }
                }

                if (debug()) {
                    std::cerr << "  Rings:\n";
                    for (auto& ring : m_rings) {
                        std::cerr << "    " << ring;
                        if (ring.closed()) {
                            std::cerr << " (closed)";
                        }
                        std::cerr << "\n";
                    }
                }

                if (check_for_open_rings()) {
                    if (debug()) {
                        std::cerr << "  not all rings are closed\n";
                    }
                    return false;
                }

                if (debug()) {
                    std::cerr << "  Find inner/outer...\n";
                }

                if (m_rings.size() == 1) {
                    m_outer_rings.push_back(&m_rings.front());
                } else {
                    for (auto& ring : m_rings) {
                        check_inner_outer(ring);
                        if (ring.outer()) {
                            if (!ring.is_cw()) {
                                ring.reverse();
                            }
                            m_outer_rings.push_back(&ring);
                        } else {
                            if (ring.is_cw()) {
                                ring.reverse();
                            }
                            m_inner_rings.push_back(&ring);
                        }
                    }

                    if (m_outer_rings.size() == 1) {
                        for (auto inner : m_inner_rings) {
                            m_outer_rings.front()->add_inner_ring(inner);
                        }
                    } else {
                        // sort outer rings by size, smallest first
                        std::sort(m_outer_rings.begin(), m_outer_rings.end(), [](ProtoRing* a, ProtoRing* b) {
                            return a->area() < b->area();
                        });
                        for (auto inner : m_inner_rings) {
                            for (auto outer : m_outer_rings) {
                                if (inner->is_in(outer)) {
                                    outer->add_inner_ring(inner);
                                    break;
                                }
                            }
                        }
                    }
                }

                check_inner_outer_roles();

                return true;
            }

        public:

            typedef osmium::area::AssemblerConfig config_type;

            Assembler(const config_type& config) :
                m_config(config),
                m_segment_list(config.debug) {
            }

            ~Assembler() = default;

            /**
             * Assemble an area from the given way.
             * The resulting area is put into the out_buffer.
             */
            void operator()(const osmium::Way& way, osmium::memory::Buffer& out_buffer) {
                if (m_config.problem_reporter) {
                    m_config.problem_reporter->set_object(osmium::item_type::way, way.id());
                }

                if (!way.ends_have_same_id()) {
                    if (m_config.problem_reporter) {
                        m_config.problem_reporter->report_duplicate_node(way.nodes().front().ref(), way.nodes().back().ref(), way.nodes().front().location());
                    }
                }

                m_segment_list.extract_segments_from_way(way, "outer");

                if (debug()) {
                    std::cerr << "\nBuild way id()=" << way.id() << " segments.size()=" << m_segment_list.size() << "\n";
                }

                // Now create the Area object and add the attributes and tags
                // from the relation.
                {
                    osmium::osm::AreaBuilder builder(out_buffer);
                    initialize_area_from_object(builder, way, 0);

                    if (create_rings()) {
                        add_tags_to_area(builder, way);
                        add_rings_to_area(builder);
                    }
                }
                out_buffer.commit();
            }

            /**
             * Assemble an area from the given relation and its members.
             * All members are to be found in the in_buffer at the offsets
             * given by the members parameter.
             * The resulting area is put into the out_buffer.
             */
            void operator()(const osmium::Relation& relation, const std::vector<size_t>& members, const osmium::memory::Buffer& in_buffer, osmium::memory::Buffer& out_buffer) {
                if (m_config.problem_reporter) {
                    m_config.problem_reporter->set_object(osmium::item_type::relation, relation.id());
                }

                m_segment_list.extract_segments_from_ways(relation, members, in_buffer);

                if (debug()) {
                    std::cerr << "\nBuild relation id()=" << relation.id() << " members.size()=" << members.size() << " segments.size()=" << m_segment_list.size() << "\n";
                }

                size_t area_offset = out_buffer.committed();

                // Now create the Area object and add the attributes and tags
                // from the relation.
                {
                    osmium::osm::AreaBuilder builder(out_buffer);
                    initialize_area_from_object(builder, relation, 1);

                    if (create_rings()) {
                        add_tags_to_area(builder, relation);
                        add_rings_to_area(builder);
                    }
                }
                out_buffer.commit();

                const osmium::TagList& area_tags = out_buffer.get<osmium::Area>(area_offset).tags(); // tags of the area we just built

                if (m_inner_outer_mismatches == 0) {
                    auto memit = relation.members().begin();
                    for (size_t offset : members) {
                        if (!std::strcmp(memit->role(), "inner")) {
                            const osmium::Way& way = in_buffer.get<const osmium::Way>(offset);
                            if (way.is_closed() && way.tags().size() > 0) {
                                osmium::tags::KeyFilter filter(true);
                                filter.add(false, "created_by").add(false, "source").add(false, "note");
                                filter.add(false, "test:id").add(false, "test:section");

                                osmium::tags::KeyFilter::iterator fi_begin(filter, way.tags().begin(), way.tags().end());
                                osmium::tags::KeyFilter::iterator fi_end(filter, way.tags().end(), way.tags().end());

                                auto d = std::distance(fi_begin, fi_end);
                                if (d > 0) {
                                    osmium::tags::KeyFilter::iterator area_fi_begin(filter, area_tags.begin(), area_tags.end());
                                    osmium::tags::KeyFilter::iterator area_fi_end(filter, area_tags.end(), area_tags.end());

                                    if (!std::equal(fi_begin, fi_end, area_fi_begin) || d != std::distance(area_fi_begin, area_fi_end)) {
                                        Assembler assembler(m_config);
                                        assembler(way, out_buffer);
                                    }
                                }
                            }
                        }
                        ++memit;
                    }
                }
            }

        }; // class Assembler

    } // namespace area

} // namespace osmium

#endif // OSMIUM_AREA_ASSEMBLER_HPP
