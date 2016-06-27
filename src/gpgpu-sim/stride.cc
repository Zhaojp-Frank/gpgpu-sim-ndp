#include <climits>
#include <random>
#include "gpu-cache.h"
#include "stride.h"
#define TABLE_SIZE 256
#define CONF_THRESHOLD 3

BasePrefetcher::BasePrefetcher(const std::string name, unsigned shader_id, cache_config &config)
	:cache(nullptr), m_name(name), blkSize(128), m_shader_id(shader_id), m_config(config)
{
	blkSize = m_config.get_line_sz();
}

void BasePrefetcher::setCache(baseline_cache *_cache) {
	printf("Constructing prefetcher of cache %s in shader %d!!!\n", name().c_str(), m_shader_id);
	assert(!cache);
	cache = _cache;
}

bool BasePrefetcher::inCache(Addr addr) const
{
	//if (cache->inCache(addr)) {
	//	return true;
	//}
	return false;
}

bool BasePrefetcher::inMissQueue(Addr addr) const
{
	//if (cache->inMissQueue(addr)) {
	//	return true;
	//}
	return false;
}

bool BasePrefetcher::samePage(Addr a, Addr b) const
{
	return false;
	//return roundDown(a, pageBytes) == roundDown(b, pageBytes);
}

StridePrefetcher::StridePrefetcher(const std::string name, unsigned shader_id, cache_config &config)
	: BasePrefetcher(name, shader_id, config),
	maxConf(7),
	minConf(0),
	threshConf(4),
	startConf(4),
	degree(1),
	pcTableAssoc(8),
	pcTableSets(4),
	pcTable(pcTableAssoc, pcTableSets),
	queueFilter(true),
	cacheSnoop(false),
	queueSize(32)
{
	pfIdentified = 0;
	pfBufferHit = 0;
	pfInCache = 0;
	pfRemovedFull = 0;
	pfIssued = 0;
}

bool StridePrefetcher::inPrefetch(Addr address) const {
	for (const mem_fetch *mf : prefetch_queue) {
		if (mf->get_addr() == address) return true;
	}
	return false;
}

void StridePrefetcher::notify(mem_fetch *mf) {
	std::vector<Addr> addresses;
	calculatePrefetch(mf, addresses);
	for (Addr pf_addr : addresses) {
		//Addr block_addr = m_config.block_addr(pf_addr);
		pfIdentified++;
		printf("Found a pf candidate addr: 0x%llx, inserting into prefetch queue.\n", pf_addr);
		if (queueFilter && inPrefetch(pf_addr)) {
			pfBufferHit++;
			printf("Prefetch addr already in prefetch queue\n");
			continue;
		}
		if (cacheSnoop && (inCache(pf_addr) || inMissQueue(pf_addr))) {
			pfInCache++;
			printf("Dropping redundant in cache/MSHR prefetch addr: 0x%llx\n", pf_addr);
			continue;
		}
		
		//Generate a memory request
		const mem_access_t *ma = new mem_access_t( GLOBAL_ACC_R,
									pf_addr,
									mf->get_data_size(),
									false,
									mf->get_access_warp_mask(),
									mf->get_access_byte_mask() );
		mem_fetch *pf_mf = new mem_fetch( *ma, NULL, 
									mf->get_ctrl_size(), 
									mf->get_wid(), 
									mf->get_sid(), 
									mf->get_tpc(), 
									mf->get_mem_config());
		pf_mf->set_is_prefetch();

		// Verify prefetch buffer space for request
		if (prefetch_queue.size() == queueSize) {
			pfRemovedFull ++;
			mem_fetch *old_mf = prefetch_queue.front();
			printf("Prefetch queue full, removing oldest request addr: 0x%llx", old_mf->get_addr());
			prefetch_queue.pop_front();
			delete old_mf;
		}
		prefetch_queue.push_back(pf_mf); // push the request into the queue
	}
}

void StridePrefetcher::popRequest() {
	//printf("Issuing a prefetch.\n");
	assert(!prefetch_queue.empty());
	prefetch_queue.pop_front();
	pfIssued++;
}

mem_fetch* StridePrefetcher::getRequest() {
	if (prefetch_queue.empty()) {
		//printf("No hardware prefetches available.\n");
		return NULL;
	} 
	mem_fetch *mf = prefetch_queue.front();
	assert(mf != NULL);
	printf("Generating prefetch for 0x%llx.\n", mf->get_addr());
	return mf;
}

StridePrefetcher::StrideEntry** StridePrefetcher::PCTable::allocateNewContext(int context) {
	auto res = entries.insert(std::make_pair(context,
				new StrideEntry*[pcTableSets]));
	auto it = res.first;
	//chatty_assert(res.second, "Allocating an already created context\n");
	assert(it->first == context);
	//printf("Adding context %i with stride entries at %p\n", context, it->second);
	StrideEntry** entry = it->second;
	for (int s = 0; s < pcTableSets; s++) {
		entry[s] = new StrideEntry[pcTableAssoc];
	}
	return entry;
}

StridePrefetcher::PCTable::~PCTable() {
	for (auto entry : entries) {
		for (int s = 0; s < pcTableSets; s++) {
			delete[] entry.second[s];
		}
		delete[] entry.second;
	}
}

inline int floorLog2(unsigned x) {
	assert(x > 0);
	int y = 0;
	if (x & 0xffff0000) { y += 16; x >>= 16; }
	if (x & 0x0000ff00) { y +=  8; x >>=  8; }
	if (x & 0x000000f0) { y +=  4; x >>=  4; }
	if (x & 0x0000000c) { y +=  2; x >>=  2; }
	if (x & 0x00000002) { y +=  1; }
	return y;
}

inline int floorLog2(int x) {
	assert(x > 0);
	return floorLog2((unsigned)x);
}

inline Addr StridePrefetcher::pcHash(Addr pc) const {
	Addr hash1 = pc >> 1;
	Addr hash2 = hash1 >> floorLog2(pcTableSets);
	return (hash1 ^ hash2) & (Addr)(pcTableSets - 1);
}

inline StridePrefetcher::StrideEntry* StridePrefetcher::pcTableVictim(Addr pc, unsigned warp_id, unsigned shader_id) {
	//int set = pcHash(pc);
	//std::mt19937_64 gen;
	//std::uniform_int_distribution<int> dist(0, pcTableAssoc - 1);
	//int way = dist(gen);
	int set = warp_id % pcTableSets;
	int way = -1;
	int least_conf = maxConf + 1;
	for(int i = 0; i < pcTableAssoc; i ++) {
		StrideEntry* entry = &pcTable[shader_id][set][i];
		assert(entry);
		// find the way with the least confidence
		if(entry->confidence < least_conf) {
			way = i;
			least_conf = entry->confidence;
		}
	}
	assert(way >=0 && way < pcTableAssoc);
	printf("Victiming table[%d][%d].\n", set, way);
	return &pcTable[shader_id][set][way];
}

inline bool StridePrefetcher::pcTableHit(Addr pc, unsigned warp_id, unsigned shader_id, StrideEntry* &entry) {
	//int set = pcHash(pc);
	int set = warp_id % pcTableSets;
	StrideEntry* set_entries = pcTable[shader_id][set];
	for (int way = 0; way < pcTableAssoc; way++) {
		if (set_entries[way].instAddr == pc &&
				set_entries[way].wid == warp_id) {
			printf("Lookup hit table[%d][%d].\n", set, way);
			entry = &set_entries[way];
			return true;
		}
	}
	return false;
}

void StridePrefetcher::calculatePrefetch(const mem_fetch *mf, std::vector<Addr> &addresses) {
	Addr mf_addr = mf->get_addr();
	address_type pc = mf->get_pc();
	unsigned warp_id = mf->get_wid();
	unsigned shader_id = m_shader_id;
	//unsigned shader_id = mf->get_sid();
	
	StrideEntry *entry;

	if(pcTableHit(pc, warp_id, shader_id, entry)) {
		// Hit in table
		int new_stride = mf_addr - entry->lastAddr;
		bool stride_match = (new_stride == entry->stride);

		// Adjust confidence for stride entry
		if (stride_match && new_stride != 0) {
			if (entry->confidence < maxConf)
				entry->confidence++;
		} else {
			if (entry->confidence > minConf)
				entry->confidence--;
			// If confidence has dropped below the threshold, train new stride
			if (entry->confidence < threshConf)
				entry->stride = new_stride;
		}

		printf("Hit in table of %s: PC 0x%x wid %d mf_addr 0x%llx stride %d (%s), conf %d\n",
				name().c_str(), pc, warp_id, mf_addr, new_stride, stride_match ? "match" : "change", entry->confidence);

		entry->lastAddr = mf_addr;

		// Abort prefetch generation if below confidence threshold
		if (entry->confidence <= threshConf)
			return;

		// Generate up to degree prefetches
		for (int d = 1; d <= degree; d++) {
			// Round strides up to atleast 1 cacheline
			int prefetch_stride = new_stride;
			int blkSize = 128;
			if (abs(new_stride) < blkSize) {
				prefetch_stride = (new_stride < 0) ? -blkSize : blkSize;
			}

			Addr new_addr = mf_addr + d * prefetch_stride;
			//if (samePage(data_addr, new_addr)) {
				//printf("queuing prefetch to %x\n", new_addr);
				addresses.push_back(new_addr);
			//} else {
				// Record the number of page crossing prefetches generated
				//pfSpanPage += degree - d + 1;
				//return;
			//}
		}
	} else {
		// Miss in table
		StrideEntry* entry = pcTableVictim(pc, warp_id, shader_id);
		entry->instAddr = pc;
		entry->lastAddr = mf_addr;
		entry->stride = 0;
		entry->wid = warp_id;
		entry->confidence = startConf;
		printf("Miss in table of %s: PC 0x%x wid %d mf_addr 0x%llx, stride %d, conf %d\n", name().c_str(), pc, warp_id, mf_addr, 0, startConf);
	}
}

