#ifndef __HASHMAP_HH__
#define __HASHMAP_HH__

#define hash_map unordered_map
#define hash_multimap unordered_multimap
#define hash_set unordered_set
#define hash_multiset unordered_multiset

#include <unordered_map>
#include <unordered_set>
#define __hash_namespace std
#define __hash_namespace_begin namespace std {
#define __hash_namespace_end }

namespace m5 {
	using ::__hash_namespace::hash_multimap;
	using ::__hash_namespace::hash_multiset;
	using ::__hash_namespace::hash_map;
	using ::__hash_namespace::hash_set;
	using ::__hash_namespace::hash;
}

#endif // __HASHMAP_HH__
