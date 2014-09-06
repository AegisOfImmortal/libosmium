#ifndef OSMIUM_INDEX_MULTIMAP_VECTOR_HPP
#define OSMIUM_INDEX_MULTIMAP_VECTOR_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2013,2014 Jochen Topf <jochen@topf.org> and others (see README).

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
#include <cstddef>
#include <utility>

#include <osmium/index/multimap.hpp>
#include <osmium/io/detail/read_write.hpp>

namespace osmium {

    namespace index {

        namespace multimap {

            template <typename TId, typename TValue, template<typename...> class TVector>
            class VectorBasedSparseMultimap : public Multimap<TId, TValue> {

            public:

                typedef typename std::pair<TId, TValue> element_type;
                typedef TVector<element_type> vector_type;
                typedef typename vector_type::iterator iterator;
                typedef typename vector_type::const_iterator const_iterator;

            private:

                vector_type m_vector;

                static bool is_removed(element_type& element) {
                    return element.second == osmium::index::empty_value<TValue>();
                }

            public:

                void set(const TId id, const TValue value) override final {
                    m_vector.push_back(element_type(id, value));
                }

                void unsorted_set(const TId id, const TValue value) {
                    m_vector.push_back(element_type(id, value));
                }

                std::pair<iterator, iterator> get_all(const TId id) {
                    const element_type element {
                        id,
                        osmium::index::empty_value<TValue>()
                    };
                    return std::equal_range(m_vector.begin(), m_vector.end(), element, [](const element_type& a, const element_type& b) {
                        return a.first < b.first;
                    });
                }

                std::pair<const_iterator, const_iterator> get_all(const TId id) const {
                    const element_type element {
                        id,
                        osmium::index::empty_value<TValue>()
                    };
                    return std::equal_range(m_vector.cbegin(), m_vector.cend(), element, [](const element_type& a, const element_type& b) {
                        return a.first < b.first;
                    });
                }

                size_t size() const override final {
                    return m_vector.size();
                }

                size_t byte_size() const {
                    return m_vector.size() * sizeof(element_type);
                }

                size_t used_memory() const override final {
                    return sizeof(element_type) * size();
                }

                void clear() override final {
                    m_vector.clear();
                    m_vector.shrink_to_fit();
                }

                void sort() override final {
                    std::sort(m_vector.begin(), m_vector.end());
                }

                void remove(const TId id, const TValue value) {
                    auto r = get_all(id);
                    for (auto it = r.first; it != r.second; ++it) {
                        if (it->second == value) {
                            it->second = 0;
                            return;
                        }
                    }
                }

                void consolidate() {
                    std::sort(m_vector.begin(), m_vector.end());
                }

                void erase_removed() {
                    m_vector.erase(
                        std::remove_if(m_vector.begin(), m_vector.end(), is_removed),
                        m_vector.end()
                    );
                }

                void dump_as_list(int fd) const {
                    osmium::io::detail::reliable_write(fd, reinterpret_cast<const char*>(m_vector.data()), byte_size());
                }

            }; // class VectorBasedSparseMultimap

        } // namespace multimap

    } // namespace index

} // namespace osmium

#endif // OSMIUM_INDEX_MULTIMAP_VECTOR_HPP
