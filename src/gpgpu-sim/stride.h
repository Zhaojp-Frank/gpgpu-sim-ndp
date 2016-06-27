#ifndef __STRIDE_PREFETCHER_HH__
#define __STRIDE_PREFETCHER_HH__

#include <unordered_map>
#include "../abstract_hardware_model.h"
#include "mem_fetch.h"
#include "hashmap.h"
#define Addr new_addr_type

class baseline_cache;
class cache_config;

class BasePrefetcher
{
	protected:
		baseline_cache* cache; // Pointr to the parent cache
		const std::string m_name;
		unsigned blkSize; // The block size of the parent cache
		unsigned m_shader_id; // shader id
		cache_config &m_config; // cache config
		bool inCache(Addr addr) const; // Determine if address is in cache
		bool inMissQueue(Addr addr) const; // Determine if address is in cache miss queue
		bool samePage(Addr a, Addr b) const; // Determine if addresses are on the same page

	public:
		BasePrefetcher(const std::string name, unsigned shader_id, cache_config &config);
		virtual ~BasePrefetcher() {}
		const std::string name() {return m_name; }
		void setCache(baseline_cache *_cache);
		virtual void notify(mem_fetch *mf) = 0;
		virtual void popRequest() = 0;
		virtual mem_fetch* getRequest() = 0;
		virtual void calculatePrefetch(const mem_fetch *mf, std::vector<Addr> &addresses) = 0;
};

class StridePrefetcher : public BasePrefetcher
{
	protected:
		const int maxConf;
		const int minConf;
		const int threshConf;
		const int startConf;
		const int degree;
		const int pcTableAssoc;
		const int pcTableSets;
		struct StrideEntry {
			StrideEntry() : instAddr(0), lastAddr(0), wid(0), stride(0), confidence(0) { }
			Addr instAddr;
			Addr lastAddr;
			unsigned wid; // warp ID
			int stride;
			int confidence;
		};

		class PCTable {   
			public:
				PCTable(int assoc, int sets) :
					pcTableAssoc(assoc), pcTableSets(sets) {}
				StrideEntry** operator[] (int context) {
					auto it = entries.find(context);
					if (it != entries.end())
						return it->second;

					return allocateNewContext(context);
				}
				~PCTable();
			private:
				const int pcTableAssoc;
				const int pcTableSets;
				const std::string m_name;
				m5::hash_map<int, StrideEntry**> entries;
				StrideEntry** allocateNewContext(int context);
		};
		PCTable pcTable;
		std::list<mem_fetch*> prefetch_queue; // Queue for prefetch requests
		const bool queueFilter; // Filter prefetches if already queued
		const bool cacheSnoop; //Snoop the cache before generating prefetch (cheating basically)
		const unsigned queueSize; // Maximum size of the prefetch_queue
		unsigned pfIdentified; // Number of prefetch requests identified by the prefetcher
		unsigned pfBufferHit; // Number of prefetch requests hit in the prefetch_queue
		unsigned pfInCache; // Number of prefetch requests hit in cache
		unsigned pfRemovedFull; // Number of prefetch requests removed from prefetch_queue because of queue full
		unsigned pfIssued; // Number of prefetch requests issued
		bool inPrefetch(Addr address) const;

		bool pcTableHit(Addr pc, unsigned warp_id, unsigned shader_id, StrideEntry* &entry);
		StrideEntry* pcTableVictim(Addr pc, unsigned warp_id, unsigned shader_id);
		Addr pcHash(Addr pc) const;

	public:
		StridePrefetcher(const std::string name, unsigned shader_id, cache_config &config);
		void notify(mem_fetch *mf);
		void popRequest();
		mem_fetch* getRequest();
		void calculatePrefetch(const mem_fetch *mf, std::vector<Addr> &addresses);
};

#endif // __STRIDE_PREFETCHER_H__
