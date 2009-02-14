#ifndef REFERENCE_H_
#define REFERENCE_H_

/**
 * Concrete reference representation that bulk-loads the reference from
 * the bit-pair-compacted binary file and stores it in memory also in
 * bit-pair-compacted format.  The user may request reference
 * characters either on a per-character bases or by "stretch" using
 * getBase(...) and getStretch(...) respectively.
 *
 * Most of the complexity in this class is due to the fact that we want
 * to represent references with ambiguous (non-A/C/G/T) characters but
 * we don't want to use more than two bits per base.  This means we
 * need a way to encode the ambiguous stretches of the reference in a
 * way that is external to the bitpair sequence.  To accomplish this,
 * we use the RefRecords vector, which is stored in the .3.ebwt index
 * file.  The bitpairs themselves are stored in the .4.ebwt index file.
 */
class BitPairReference {

public:
	/**
	 * Load from .3.ebwt/.4.ebwt Bowtie index files.
	 */
	BitPairReference(const string& in,
	                 bool sanity = false,
	                 std::vector<string>* infiles = NULL,
	                 std::vector<String<Dna5> >* origs = NULL,
	                 bool infilesSeq = false,
	                 bool useShmem = false) :
	loaded_(true),
	sanity_(sanity),
	useShmem_(useShmem)
	{
		string s3 = in + ".3.ebwt";
		string s4 = in + ".4.ebwt";
		FILE *f3 = fopen(s3.c_str(), "rb");
		if(f3 == NULL) {
			cerr << "Could not open reference-string index file " << s3 << " for reading." << endl;
			cerr << "This is most likely because your index was built with an older version" << endl
			     << "(<= 0.9.8.1) of bowtie-build.  Please re-run bowtie-build to generate a new" << endl
			     << "index (or download one from the Bowtie website) and try again." << endl;
			loaded_ = false;
			return;
		}
		// Read endianness sentinel, set 'swap'
		uint32_t one;
		bool swap = false;
		if(!fread(&one, 4, 1, f3)) {
			cerr << "Error reading first word from reference-structure index file " << s3 << endl;
			exit(1);
		}
		if(one != 1) {
			assert_eq(0x1000000, one);
			swap = true; // have to endian swap U32s
		}
		// Read # records
		uint32_t sz;
		if(!fread(&sz, 4, 1, f3)) {
			cerr << "Error reading # records from reference-structure index file " << s3 << endl;
			exit(1);
		}
		if(swap) sz = endianSwapU32(sz);
		// Read records
		nrefs_ = 0;
		// Cumulative count of all unambiguous characters on a per-
		// stretch 8-bit alignment (i.e. count of bytes we need to
		// allocate in buf_)
		uint32_t cumsz = 0;
		uint32_t cumlen = 0;
		// For each unambiguous stretch...
		for(uint32_t i = 0; i < sz; i++) {
			recs_.push_back(RefRecord(f3, swap));
			if(recs_.back().first) {
				// Remember that this is the first record for this
				// reference sequence (and the last record for the one
				// before)
				refRecOffs_.push_back(recs_.size()-1);
				refOffs_.push_back(cumsz);
				if(nrefs_ > 0) {
					refLens_.push_back(cumlen);
				}
				cumlen = 0;
				nrefs_++;
			}
			cumsz += recs_.back().len;
			cumlen += recs_.back().off;
			cumlen += recs_.back().len;
		}
		// Store a cap entry for the end of the last reference seq
		refRecOffs_.push_back(recs_.size());
		refOffs_.push_back(cumsz);
		refLens_.push_back(cumlen);
		bufSz_ = cumsz;
		assert_eq(nrefs_, refLens_.size());
		assert_eq(sz, recs_.size());
		fclose(f3); // done with .3.ebwt file
		// Round cumsz up to nearest byte boundary
		if((cumsz & 3) != 0) {
			cumsz += (4 - (cumsz & 3));
		}
		assert_eq(0, cumsz & 3); // should be rounded up to nearest 4
		FILE *f4 = fopen(s4.c_str(), "rb");
		if(f4 == NULL) {
			cerr << "Could not open reference-string index file " << s4 << " for reading." << endl;
			cerr << "This is most likely because your index was built with an older version" << endl
			     << "(<= 0.9.8.1) of bowtie-build.  Please re-run bowtie-build to generate a new" << endl
			     << "index (or download one from the Bowtie website) and try again." << endl;
			loaded_ = false;
			return;
		}
		// Allocate a buffer to hold the whole reference string
		buf_ = new uint8_t[cumsz >> 2];
		// Read the whole thing in
		size_t ret = fread(buf_, 1, cumsz >> 2, f4);
		// Didn't read all of it?
		if(ret != (cumsz >> 2)) {
			cerr << "Only read " << ret << " bytes (out of " << (cumsz >> 2) << ") from reference index file " << s4 << endl;
			exit(1);
		}
		char c;
		// Make sure there's no more
		ret = fread(&c, 1, 1, f4);
		assert_eq(0, ret); // should have failed
		fclose(f4);
#ifndef NDEBUG
		if(sanity_) {
			// Compare the sequence we just read from the compact index
			// file to the true reference sequence.
			std::vector<seqan::String<seqan::Dna5> > *os; // for holding references
			std::vector<seqan::String<seqan::Dna5> > osv; // for holding references
			if(infiles != NULL) {
				if(infilesSeq) {
					for(size_t i = 0; i < infiles->size(); i++) {
						// Remove initial backslash; that's almost
						// certainly being used to protect the first
						// character of the sequence from getopts (e.g.,
						// when the first char is -)
						if((*infiles)[i].at(0) == '\\') {
							(*infiles)[i].erase(0, 1);
						}
						osv.push_back(String<Dna5>((*infiles)[i]));
					}
				} else {
					readSequenceFiles<seqan::String<seqan::Dna5>, seqan::Fasta>(*infiles, osv);
				}
				os = &osv;
			} else {
				assert(origs != NULL);
				os = origs;
			}
			for(size_t i = 0; i < os->size(); i++) {
				size_t olen = seqan::length((*os)[i]);
				uint8_t *buf = new uint8_t[olen];
				getStretch(buf, i, 0, olen);
				for(size_t j = 0; j < olen; j++) {
					assert_eq((*os)[i][j], buf[j]);
					assert_eq((int)(*os)[i][j], getBase(i, j));
				}
				delete[] buf;
			}
		}
#endif
	}

	~BitPairReference() {
		delete[] buf_;
	}

	/**
	 * Return a single base of the reference.  Calling this repeatedly
	 * is not an efficient way to retrieve bases from the reference;
	 * use loadStretch() instead.
	 *
	 * This implementation scans linearly through the records for the
	 * unambiguous stretches of the target reference sequence.  When
	 * there are many records, binary search would be more appropriate.
	 */
	int getBase(uint32_t tidx, uint32_t toff) const {
		uint32_t reci = refRecOffs_[tidx];   // first record for target reference sequence
		uint32_t recf = refRecOffs_[tidx+1]; // last record (exclusive) for target seq
		assert_gt(recf, reci);
		uint32_t bufOff = refOffs_[tidx];
		uint32_t off = 0;
		// For all records pertaining to the target reference sequence...
		for(uint32_t i = reci; i < recf; i++) {
			assert_geq(toff, off);
			off += recs_[i].off;
			if(toff < off) {
				return 4;
			}
			assert_geq(toff, off);
			uint32_t recOff = off + recs_[i].len;
			if(toff < recOff) {
				toff -= off;
				bufOff += toff;
				assert_lt(bufOff, refOffs_[tidx+1]);
				const uint32_t bufElt = (bufOff) >> 2;
				assert_lt(bufElt, bufSz_);
				const uint32_t shift = (bufOff & 3) << 1;
				return ((buf_[bufElt] >> shift) & 3);
			}
			bufOff += recs_[i].len;
			off = recOff;
			assert_geq(toff, off);
		} // end for loop over records
		return 4;
	}

	/**
	 * Load a stretch of the reference string into memory at 'dest'.
	 *
	 * This implementation scans linearly through the records for the
	 * unambiguous stretches of the target reference sequence.  When
	 * there are many records, binary search would be more appropriate.
	 */
	void getStretch(uint8_t *dest,
	                uint32_t tidx,
	                uint32_t toff,
	                uint32_t count) const
	{
		uint32_t reci = refRecOffs_[tidx];   // first record for target reference sequence
		uint32_t recf = refRecOffs_[tidx+1]; // last record (exclusive) for target seq
		assert_gt(recf, reci);
		uint32_t cur = 0;
		uint32_t bufOff = refOffs_[tidx];
		uint32_t off = 0;
		// For all records pertaining to the target reference sequence...
		for(uint32_t i = reci; i < recf; i++) {
			assert_geq(toff, off);
			off += recs_[i].off;
			for(; toff < off && count > 0; toff++) {
				dest[cur++] = 4;
				count--;
			}
			if(count == 0) break;
			assert_geq(toff, off);
			bufOff += (toff - off); // move bufOff pointer forward
			off += recs_[i].len;
			for(; toff < off && count > 0; toff++) {
				assert_lt(bufOff, refOffs_[tidx+1]);
				const uint32_t bufElt = (bufOff) >> 2;
				assert_lt(bufElt, bufSz_);
				const uint32_t shift = (bufOff & 3) << 1;
				dest[cur++] = (buf_[bufElt] >> shift) & 3;
				bufOff++;
				count--;
			}
			if(count == 0) break;
			assert_geq(toff, off);
		} // end for loop over records
		// In any chars are left after scanning all the records,
		// they must be ambiguous
		while(count > 0) {
			count--;
			dest[cur++] = 4;
		}
		assert_eq(0, count);
	}

	/// Return the number of reference sequences.
	uint32_t numRefs() const {
		return nrefs_;
	}

	/// Return the number of reference sequences.
	uint32_t approxLen(uint32_t elt) const {
		assert_lt(elt, nrefs_);
		return refLens_[elt];
	}

	/// Return true iff buf_ and all the vectors are populated.
	bool loaded() const {
		return loaded_;
	}

protected:
	std::vector<RefRecord> recs_;       /// records describing unambiguous stretches
	std::vector<uint32_t>  refLens_;    /// approx lens of ref seqs (excludes trailing ambig chars)
	std::vector<uint32_t>  refOffs_;    /// buf_ begin offsets per ref seq
	std::vector<uint32_t>  refRecOffs_; /// record begin/end offsets per ref seq
	uint8_t *buf_;      /// the whole reference as a big bitpacked byte array
	uint32_t bufSz_;    /// size of buf_
	uint32_t nrefs_;    /// the number of reference sequences
	bool     loaded_;   /// whether it's loaded
	bool     sanity_;   /// do sanity checking
	bool     useShmem_; /// put the cache memory in shared memory
};

#endif
