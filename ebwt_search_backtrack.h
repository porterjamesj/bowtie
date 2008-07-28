#ifndef EBWT_SEARCH_BACKTRACK_H_
#define EBWT_SEARCH_BACKTRACK_H_

#define DEFAULT_SPREAD 64

/// Encapsulates a change made to a query base, i.e. "the 3rd base from
/// the 5' end was changed from an A to a T".  Useful when using
/// for matching seeded by "seedlings".
struct QueryMutation {
	QueryMutation(uint8_t _pos, uint8_t _oldBase, uint8_t _newBase) :
		pos(_pos), oldBase(_oldBase), newBase(_newBase)
	{
		assert_neq(oldBase, newBase);
		assert_lt(oldBase, 4);
		assert_lt(newBase, 4);
	}
	uint8_t pos;
	uint8_t oldBase;
	uint8_t newBase;
};

/**
 * Class that coordinates quality- and quantity-aware backtracking over
 * some range of a read sequence.
 * 
 * The creator can configure the BacktrackManager to treat different
 * stretches of the read differently.
 */
template<typename TStr>
class BacktrackManager {
public:
	BacktrackManager(const Ebwt<TStr>& __ebwt,
	                 const EbwtSearchParams<TStr>& __params,
	                 uint32_t __unrevOff, // size of unrevisitable chunk
	                 uint32_t __1revOff,  // size of 1-revisitable chunk
	                 uint32_t __2revOff,  // size of 2-revisitable chunk
	                 uint32_t __itop,
	                 uint32_t __ibot,
	                 uint32_t __qualThresh,
	                 uint32_t __qualWobble,
	                 uint32_t __reportSeedlings = 0,
	                 String<uint8_t>* __seedlings = NULL,
	                 String<QueryMutation>* __muts = NULL,
	                 bool __verbose = true,
	                 bool __oneHit = true,
	                 uint32_t seed = 0,
	                 vector<TStr>* __os = NULL,
	                 bool __halfAndHalf = false, // hacky way of supporting separate 1-revisitable regions
	                 TStr* __qry = NULL,
	                 String<char>* __qual = NULL,
	                 String<char>* __name = NULL) :
		_qry(__qry),
		_qlen(0),
		_qual(__qual),
		_name(__name),
		_ebwt(__ebwt),
		_params(__params),
		_unrevOff(__unrevOff),
		_1revOff(__1revOff),
		_2revOff(__2revOff),
		_itop(__itop),
		_ibot(__ibot),
		_spread(DEFAULT_SPREAD),
		_maxStackDepth(DEFAULT_SPREAD),
		_qualThresh(__qualThresh),
		_qualWobble(__qualWobble),
		_oneHit(__oneHit),
		_reportOnHit(true),
		_pairs(NULL),
		_elims(NULL),
		_mms(NULL),
		_chars(NULL),
		_reportSeedlings(__reportSeedlings),
		_seedlings(__seedlings),
		_muts(__muts),
		_os(__os),
		_halfAndHalf(__halfAndHalf),
		_5depth(__1revOff),
		_3depth(__2revOff),
		_nameDefault("default"),
		_rand(RandomSource(seed)),
		_verbose(__verbose)
	{
	    // For a 40-bp query range, the _pairs array occupies
	    // 40 * 40 * 8 * 4 = 51,200 bytes, and _elims
	    // occupy 40 * 40 = 1,600 bytes
	    assert_geq(__1revOff, __unrevOff);
	    assert_geq(__2revOff, __unrevOff);
	    fill(_qualDefault, DEFAULT_SPREAD, (char)(40+33));
 		if(_qry != NULL) {
 			_qlen = length(*_qry);
 			_spread = length(*_qry);
 			if(_qual == NULL || length(*_qual) == 0) {
 				_qual = &_qualDefault;
 			}
 			assert_geq(length(*_qual), _qlen);
 			for(size_t i = 0; i < length(*_qual); i++) {
 				assert_geq((*_qual)[i], 33);
 				assert_leq((*_qual)[i], 73);
 			}
 			assert_geq(length(*_qual), _qlen);
 			if(_name == NULL || length(*_name) == 0) {
 				_name = &_nameDefault;
 			}
 			assert_leq(_spread, DEFAULT_SPREAD);
 	 		_maxStackDepth = length(*_qry) - min(_unrevOff, length(*_qry)) + 3 + 1;
 	 		_pairs  = new uint32_t[DEFAULT_SPREAD*_maxStackDepth*8];
 	 		_elims  = new uint8_t [DEFAULT_SPREAD*_maxStackDepth];
 			if(_muts != NULL) {
 				applyMutations();
 			}
 		}
		if(_itop != 0 || _ibot != 0) {
			assert_lt(_itop, _ibot);
		}
		_mms = new uint32_t[DEFAULT_SPREAD];
		_chars = new char[DEFAULT_SPREAD];
	}

	~BacktrackManager() {
		if(_pairs != NULL) {
			delete[] _pairs; _pairs = NULL;
		}
		if(_elims != NULL) {
			delete[] _elims; _elims = NULL;
		}
		if(_mms != NULL) {
			delete[] _mms; _mms = NULL;
		}
		if(_chars != NULL) {
			delete[] _chars; _chars = NULL;
		}
	}

	#define PAIR_TOP(d, c)    (pairs[d*8 + c + 0])
	#define PAIR_BOT(d, c)    (pairs[d*8 + c + 4])
	#define PAIR_SPREAD(d, c) (PAIR_BOT(d, c) - PAIR_TOP(d, c))
	#define QUAL(k)           ((uint8_t)(*_qual)[k] >= 33 ? ((uint8_t)(*_qual)[k] - 33) : 0)
	#define QUAL2(q, k)       ((uint8_t)(q)[k] >= 33 ? ((uint8_t)(q)[k] - 33) : 0)
	
	void setQuery(TStr* __qry,
	              String<char>* __qual,
	              String<char>* __name,
	              String<QueryMutation>* __muts = NULL)
	{
		_qry = __qry;
		_qual = __qual;
		_name = __name;
		if(_muts != NULL) {
			undoMutations();
		}
		_muts = __muts;
		if(_muts != NULL) {
			applyMutations();
		}
		assert(_qry != NULL);
		// Reset _qlen
		_qlen = length(*_qry);
		_spread = _qlen;
		assert_leq(_spread, DEFAULT_SPREAD);
		if(_qual == NULL || length(*_qual) == 0) {
			_qual = &_qualDefault;
		}
		assert_geq(length(*_qual), _qlen);
		for(size_t i = 0; i < length(*_qual); i++) {
			assert_geq((*_qual)[i], 33);
			assert_leq((*_qual)[i], 73);
		}
		if(_name == NULL || length(*_name) == 0) {
			_name = &_nameDefault;
		}
 		_maxStackDepth = length(*_qry) - min(_unrevOff, length(*_qry)) + 3 + 1;
 		if(_pairs == NULL) {
 			_pairs = new uint32_t[DEFAULT_SPREAD*_maxStackDepth*8];
 		}
 		if(_elims == NULL) {
 			_elims = new uint8_t[DEFAULT_SPREAD*_maxStackDepth];
 		}
		if(_verbose) {
			String<char> qual = (*_qual);
			if(length(qual) > length(*_qry)) {
				resize(qual, length(*_qry));
			}
			cout << "setQuery(_qry=" << (*_qry) << ", _qual=" << qual << ")" << endl;
		}
	}
	
	void setMuts(String<QueryMutation>* __muts) {
		if(_muts != NULL) {
			// Undo previous mutations
			assert_gt(length(*_muts), 0);
			undoMutations();
		}
		_muts = __muts;
		if(_muts != NULL) {
			assert_gt(length(*_muts), 0);
			applyMutations();
		}
	}
	
	/**
	 * Set the depth before which no backtracks are allowed.
	 */
	uint32_t setUnrevOff(uint32_t unrevOff) {
		uint32_t tmp = _unrevOff;
		_unrevOff = unrevOff;
		return tmp;
	}

	uint32_t set1RevOff(uint32_t oneRevOff) {
		uint32_t tmp = _1revOff;
		_1revOff = oneRevOff;
		return tmp;
	}

	uint32_t set2RevOff(uint32_t twoRevOff) {
		uint32_t tmp = _2revOff;
		_2revOff = twoRevOff;
		return tmp;
	}

	/**
	 * Set _qlen according to parameter, except don't let it fall below
	 * the length of the query.
	 */
	void setQlen(uint32_t qlen) {
		assert(_qry != NULL);
		_qlen = min(length(*_qry), qlen);
	}

	/**
	 * Initiate the recursive backtracking routine starting at the
	 * extreme right-hand side of the pattern.  Use the ftab to match
	 * the first several characters in one chomp, as long as doing so
	 * does not "jump over" any legal backtracking targets.
	 */
	bool backtrack(uint32_t ham = 0) {
		assert_gt(length(*_qry), 0);
		assert_leq(_qlen, length(*_qry));
		assert_geq(length(*_qual), length(*_qry));
		int ftabChars = _ebwt._eh._ftabChars;
		uint32_t m = min<uint32_t>(_unrevOff, _qlen);
		if(m >= (uint32_t)ftabChars) {
			// The ftab doesn't extend past the unrevisitable portion,
			// so we can go ahead and use it
			// Rightmost char gets least significant bit-pair
			uint32_t ftabOff = (*_qry)[_qlen - ftabChars];
			assert_lt(ftabOff, _ebwt._eh._ftabLen-1);
			for(int i = ftabChars - 1; i > 0; i--) {
				ftabOff <<= 2;
				assert_lt((uint32_t)(*_qry)[_qlen-i], 4);
				ftabOff |= (uint32_t)(*_qry)[_qlen-i];
				assert_lt(ftabOff, _ebwt._eh._ftabLen-1);
			}
			assert_lt(ftabOff, _ebwt._eh._ftabLen-1);
			uint32_t top = _ebwt.ftabHi(ftabOff);
			uint32_t bot = _ebwt.ftabLo(ftabOff+1);
			if(_qlen == (uint32_t)ftabChars && bot > top) {
				// We have a match!
				if(_reportSeedlings > 0) {
					// Oops - we're trying to find seedlings, so we've
					// gone too far; start again
					return backtrack(0,   // depth
					                 0,   // top
					                 0,   // bot
					                 ham);
				} else {
					// We have a match!
					return report(0, top, bot);
				}
			} else if (bot > top) {
				// We have an arrow pair from which we can backtrack
				return backtrack(ftabChars, // depth
				                 top,       // top
				                 bot,       // bot
				                 ham);
			}
			// The arrows are already closed; give up
			return false;
		} else {
			// The ftab *does* extend past the unrevisitable portion;
			// we can't use it in this case, because we might jump past
			// a legitimate mismatch
			return backtrack(0,   // depth
			                 0,   // top
			                 0,   // bot
			                 ham);
		}
	}

	/**
	 * Starting at the given "depth" relative to the 5' end, and the
	 * given top and bot arrows (where top=0 and bot=0 means it's up to
	 * us to calculate the initial arrow pair), and initial weighted
	 * hamming distance iham, find a hit using randomized, quality-
	 * aware backtracking. 
	 */
	bool backtrack(uint32_t depth,
	               uint32_t top,
	               uint32_t bot,
	               uint32_t iham = 0)
	{
		// Initial sanity checking
		assert_gt(length(*_qry), 0);
		assert_leq(_qlen, length(*_qry));
		assert_geq(length(*_qual), length(*_qry));
		if(_verbose) cout << "backtrack(top=" << top << ", "
		                 << "bot=" << bot << ", "
		                 << "iham=" << iham << ", "
		                 << "_pairs" << _pairs << ", "
		                 << "_elims=" << (void*)_elims << ")" << endl;
		bool oldRetain = _params.sink().retainHits();
		size_t oldRetainSz = 0;
		if(_os != NULL && (*_os).size() > 0) {
			// Save some info about hits retained at this point
			oldRetainSz = _params.sink().retainedHits().size();
			_params.sink().setRetainHits(true);
		}
		ASSERT_ONLY(uint64_t nhits = _params.sink().numHits());
		
		// Initiate the recursive, randomized quality-aware backtracker
		// with a stack depth of 0 (no backtracks so far)
		bool ret = backtrack(0, depth, _unrevOff, _1revOff, _2revOff,
		                     top, bot, iham, iham, _pairs, _elims);

		// Remainder of this function is sanity checking
		
		if(ret) {
			// Return value of true implies there should be a fresh hit
			assert_eq(_params.sink().numHits(), nhits+1);
		} else {
			// Return value of false implies no new hits
			assert_eq(_params.sink().numHits(), nhits);
		}
		_params.sink().setRetainHits(oldRetain); // restore old value
		// If we have the original texts, then we double-check the
		// backtracking result against the naive oracle
		// TODO: also check seedling hits
		if(_os != NULL && (*_os).size() > 0 && _reportSeedlings == 0) {
			vector<Hit> oracleHits;
			// Invoke the naive oracle, which will place all qualifying
			// hits in the 'oracleHits' vector
			naiveOracle(oracleHits, iham);
			vector<Hit>& retainedHits = _params.sink().retainedHits();
			if(ret == false) {
				// If we didn't find any hits, then the oracle had
				// better not have found any either
				assert_eq(oldRetainSz, retainedHits.size());
				if(oracleHits.size() > 0) {
					// Oops, the oracle found at least one hit; print
					// detailed info about the first oracle hit (for
					// debugging)
					const Hit& h = oracleHits[0];
					cout << "Oracle hit " << oracleHits.size()
					     << " times, but backtracker did not hit" << endl;
					cout << "First oracle hit: " << endl;
					if(_muts != NULL) {
						undoMutations();
						cout << "  Unmutated Pat:  " << (*_qry) << endl;
						applyMutations();
					}
					cout << "  Pat:            " << (*_qry) << endl;
					cout << "  Tseg:           ";
					bool ebwtFw = _params.ebwtFw();
					if(ebwtFw) {
						for(size_t i = 0; i < _qlen; i++) {
							cout << (*_os)[h.h.first][h.h.second + i];
						}
					} else {
						for(int i = (int)_qlen-1; i >= 0; i--) {
							cout << (*_os)[h.h.first][h.h.second + i];
						}
					}
					cout << endl;
					cout << "  Quals:          " << (*_qual) << endl;
					cout << "  Bt:             ";
					for(int i = (int)_qlen-1; i >= 0; i--) {
						if     (i < (int)_unrevOff) cout << "0";
						else if(i < (int)_1revOff)  cout << "1";
						else if(i < (int)_2revOff)  cout << "2";
						else cout << "X";
					}
					cout << endl;
				}
				assert_eq(0, oracleHits.size());
			} else {
				// If we found a hit, it had better be one of the ones
				// that the oracle found
				assert_gt(oracleHits.size(), 0);
				assert_eq(oldRetainSz+1, retainedHits.size());
				// Get the hit reported by the backtracker
				Hit& rhit = retainedHits.back();
				// Go through oracleHits and look for a match
				size_t i;
				for(i = 0; i < oracleHits.size(); i++) {
					const Hit& h = oracleHits[i];
		    		if(h.h.first == rhit.h.first && h.h.second == rhit.h.second) {
		    			assert_eq(h.fw, rhit.fw);
		    			assert_eq(h.mms, rhit.mms);
		    			// It's a match - hit confirmed
		    			break;
		    		}
				}
				assert_lt(i, oracleHits.size()); // assert we found a matchup
			}
		}
		return ret;
	}

	/**
	 * Recursive routine for progressing to the next backtracking
	 * decision given some initial conditions.  If a hit is found, it
	 * is recorded and true is returned.  Otherwise, if there are more
	 * backtracking opportunities, the function will call itself
	 * recursively and return the result.  As soon as there is a
	 * mismatch and no backtracking opporunities, false is returned.
	 */
	bool backtrack(uint32_t  stackDepth, // depth of the recursion stack; = # mismatches so far
	               uint32_t  depth,    // next depth where a post-pair needs to be calculated
	               uint32_t  unrevOff, // depths < unrevOff are unrevisitable 
	               uint32_t  oneRevOff,// depths < oneRevOff are 1-revisitable 
	               uint32_t  twoRevOff,// depths < twoRevOff are 2-revisitable 
	               uint32_t  top,      // top arrow in pair prior to 'depth'
	               uint32_t  bot,      // bottom arrow in pair prior to 'depth'
	               uint32_t  ham,      // weighted hamming distance so far
	               uint32_t  iham,     // initial weighted hamming distance
	               uint32_t* pairs,    // portion of pairs array to be used for this backtrack frame
	               uint8_t*  elims)    // portion of elims array to be used for this backtrack frame
	{
		// Can't have already exceeded weighted hamming distance threshold
		assert_leq(stackDepth, depth);
		assert_gt(length(*_qry), 0);
		assert_leq(_qlen, length(*_qry));
		assert_geq(length(*_qual), length(*_qry));
		assert(_qry != NULL);
		assert(_qual != NULL);
		assert(_name != NULL);
		assert(_qlen != 0);
		assert_leq(ham, _qualThresh);
		assert_lt(depth, _qlen); // can't have run off the end of qry
		assert_geq(bot, top);    // could be that both are 0
		assert(pairs != NULL);
		assert(elims != NULL);
		assert_leq(stackDepth, _maxStackDepth);
		if(_halfAndHalf) {
			assert_eq(0, _reportSeedlings);
			assert_gt(_3depth, _5depth);
		}
		if(_reportSeedlings) {
			assert(!_halfAndHalf);
		}
		if(_verbose) {
			cout << "  backtrack(stackDepth=" << stackDepth << ", "
                 << "depth=" << depth << ", "
                 << "top=" << top << ", "
                 << "bot=" << bot << ", "
                 << "ham=" << ham << ", "
                 << "iham=" << iham << ", "
                 << "pairs=" << pairs << ", "
                 << "elims=" << (void*)elims << "): \"";
			for(int i = (int)depth - 1; i >= 0; i--) {
				cout << _chars[i];
			}
			cout << "\"" << endl;
		}
		
		// If we're searching for a half-and-half solution (exactly one
		// mismatch in both the 5' and the 3' half of the seed), then
		// enforce the half-and-half constraint here
		if(_halfAndHalf) {
			assert_eq(0, _reportSeedlings);
			if(depth == _5depth) {
				// The backtracking logic should have prevented us from
				// backtracking more than once into this region
				assert_leq(stackDepth, 1);
				// Must have encountered a mismatch by this point
				if(stackDepth < 1) return false;
			}
			else if(depth == _3depth) {
				// The backtracking logic should have prevented us from
				// backtracking more than twice within this region
				assert_leq(stackDepth, 2);
				// Must have encountered two mismatches by this point
				if(stackDepth < 2) return false;
			}
			if(depth >= _5depth) {
				assert_geq(stackDepth, 1);
			} else if(depth >= _3depth) {
				assert_geq(stackDepth, 2);
			}
		}
		
		// The total number of arrow pairs that are acceptable
		// backtracking targets ("alternative" arrow pairs)
		uint32_t altNum = 0;
		// The total number of arrow pairs that are candidates to be
		// the *next* backtracking target because they are low quality
		// ("eligible" arrow pairs)
		uint32_t eligibleNum = 0;
		// Total distance between all lowest-quality "alternative"
		// arrow pairs that haven't yet been eliminated
		uint32_t eligibleSz = 0;
		// The lowest quality value associated with any alternative
		// arrow pairs; all alternative pairs with this quality are
		// eligible
		uint8_t lowAltQual = 0xff;
		uint32_t d = depth;
		uint32_t cur = _qlen - d - 1; // current offset into _qry
		SideLocus ltop, lbot;
		if(top != 0 || bot != 0) {
			SideLocus::initFromTopBot(top, bot, _ebwt._eh, _ebwt._ebwt, ltop, lbot);
		}
		while(cur < _qlen) {
			// Try to advance further given that 
			if(_verbose) {
				cout << "    cur=" << cur << " \"";
				for(int i = (int)d - 1; i >= 0; i--) {
					cout << _chars[i];
				}
				cout << "\"";
			}
			assert_lt((int)(*_qry)[cur], 4);
			bool curIsEligible = false;
			// Reset eligibleNum and eligibleSz if there are any
			// eligible pairs discovered at this spot
			bool curOverridesEligible = false;
			// Determine whether arrow pairs at this location are
			// candidates for backtracking
			int c = (int)(*_qry)[cur];
			uint8_t q = QUAL(cur);
			assert_lt((uint32_t)q, 100);
			bool curIsAlternative = (d >= unrevOff) && (ham + q <= _qualThresh);
			if(curIsAlternative) {
				// q is low enough to make this position an alternative,
				// but is it the best alternative?
				if(q < lowAltQual) {
					// Arrow pairs at this depth in this backtracking frame
					// are eligible, unless we learn otherwise.  Arrow
					// pairs previously thought to be eligible are not any
					// longer.
					curIsEligible = true;
					curOverridesEligible = true;
				} else if(q == lowAltQual) {
					// Arrow pairs at this depth in this backtracking frame
					// are eligible, unless we learn otherwise
					curIsEligible = true;
				}
			}
			if(curIsEligible) assert(curIsAlternative);
			if(curOverridesEligible) assert(curIsEligible);
			if(curIsAlternative && !curIsEligible) {
				assert_gt(eligibleSz, 0);
				assert_gt(eligibleNum, 0);
			}
			if(_verbose) {
				cout << " alternative: " << curIsAlternative;
				cout << ", eligible: " << curIsEligible;
				if(curOverridesEligible) cout << "(overrides)";
				cout << endl;
			}
			if(top == 0 && bot == 0) {
				// Calculate first quartet of pairs using the _fchr[]
				// array
				assert_eq(0, d);
				PAIR_TOP(0, 0)                  = _ebwt._fchr[0];
				PAIR_BOT(0, 0) = PAIR_TOP(0, 1) = _ebwt._fchr[1];
				PAIR_BOT(0, 1) = PAIR_TOP(0, 2) = _ebwt._fchr[2];
				PAIR_BOT(0, 2) = PAIR_TOP(0, 3) = _ebwt._fchr[3];
				PAIR_BOT(0, 3)                  = _ebwt._fchr[4];
				// Update top and bot
				top = PAIR_TOP(d, c); bot = PAIR_BOT(d, c);
			} else if(curIsAlternative) {
				// Clear pairs
				bzero(&pairs[d*8], 8 * 4);
				// Calculate next quartet of pairs
				_ebwt.mapLFEx(ltop, lbot, &pairs[d*8], &pairs[(d*8)+4]);
				// Update top and bot
				top = PAIR_TOP(d, c); bot = PAIR_BOT(d, c);
			} else {
				// This query character is not even a legitimate
				// alternative (because backtracking here would blow
				// our mismatch quality budget), so no need to do the
				// bookkeeping for the entire quartet, just do c
				top = _ebwt.mapLF(ltop, c); bot = _ebwt.mapLF(lbot, c);
			}
			if(top != bot) {
				// Calculate loci from row indices; do it now so that
				// those prefetches are fired off as soon as possible
				SideLocus::initFromTopBot(top, bot, _ebwt._eh, _ebwt._ebwt, ltop, lbot);
			}
			// Update the elim array
			elims[d] = (1 << c);
			assert_lt(elims[d], 16);
			assert_gt(elims[d], 0);
			if(curIsAlternative) {
				// Given the just-calculated quartet of arrow pairs, update
				// elims, altNum, eligibleNum, eligibleSz
				for(int i = 0; i < 4; i++) {
					assert_leq(PAIR_TOP(d, i), PAIR_BOT(d, i));
					uint32_t spread = PAIR_SPREAD(d, i);
					if(spread == 0) {
						// Indicate this char at this position is
						// eliminated as far as this backtracking frame is
						// concerned, since its arrow pair is closed
						elims[d] |= (1 << i);
					}
					if(i != c && spread > 0 && ((elims[d] & (1 << i)) == 0)) {
						if(curIsEligible) {
							if(curOverridesEligible) {
								// Only now that we know there is at least
								// one potential backtrack target at this
								// most-eligible position should we reset
								// these eligibility parameters
								lowAltQual = q;
								eligibleNum = 0;
								eligibleSz = 0;
								curOverridesEligible = false;
							}
							eligibleSz += spread;
							eligibleNum++;
						}
						assert_gt(eligibleSz, 0);
						assert_gt(eligibleNum, 0);
						altNum++;
					}
				}
			}
			if(altNum > 0) {
				assert_gt(eligibleSz, 0);
				assert_gt(eligibleNum, 0);
			}
			assert_leq(eligibleNum, eligibleSz);
			assert_leq(eligibleNum, altNum);
			assert_lt(elims[d], 16);
			assert(sanityCheckEligibility(depth, d, unrevOff, lowAltQual, eligibleSz, eligibleNum, pairs, elims));
			// Achieved a match, but need to keep going
			bool keepGoingDespiteMatch = false;
			if(cur == 0 &&  // we've consumed the entire pattern
			   top < bot && // there's a hit to report
			   stackDepth < _reportSeedlings && // not yet used up our mismatches
			   _reportSeedlings > 0 &&
			   altNum > 0)  // there are still legel backtracking targets
			{
				assert(!_halfAndHalf);
				keepGoingDespiteMatch = true;
				if(stackDepth > 0) {
					// This is a legit seedling; report it
					reportSeedling(stackDepth);
				}
				// Now continue on to find legitimate seedlings with
				// more mismatches than this one
			}
			else if(_halfAndHalf && (d == (_5depth-1)) && top < bot) {
				// About to transition into the 3' half of the seed;
				// we should induce a mismatch if we haven't mismatched
				// yet, so that we don't waste time pursuing a match
				// that was covered by a previous phase
				assert_eq(0, _reportSeedlings);
				if(stackDepth == 0) {
					keepGoingDespiteMatch = true;
				} else {
					assert_eq(1, stackDepth);
				}
			}
			else if(_halfAndHalf && (d == (_3depth-1)) && top < bot) {
				// About to transition into the 3' half of the seed;
				// we should induce a mismatch if we haven't mismatched
				// yet, so that we don't waste time pursuing a match
				// that was covered by a previous phase
				assert_eq(0, _reportSeedlings);
				if(stackDepth < 2) {
					keepGoingDespiteMatch = true;
				} else {
					assert_eq(2, stackDepth);
				}
			}
			// Mismatch with alternatives
			while((top == bot && altNum > 0) || keepGoingDespiteMatch) {
				keepGoingDespiteMatch = false;
				if(_verbose) cout << "    top (" << top << ") == bot ("
				                 << bot << ") with " << altNum
				                 << " alternatives, eligible: "
				                 << eligibleNum << ", " << eligibleSz
				                 << endl;
				assert_gt(eligibleSz, 0);
				assert_gt(eligibleNum, 0);
				// Mismatch!  Must now choose where we are going to
				// take our quality penalty.  We can only look as far
				// back as our last decision point.
				assert(sanityCheckEligibility(depth, d, unrevOff, lowAltQual, eligibleSz, eligibleNum, pairs, elims));
				// Pick out the arrow pair we selected and target it
				// for backtracking
				// TODO: optimize for (relatively common) case where
				// eligibleSz == 1
				uint32_t r = _rand.nextU32() % eligibleSz;
				bool foundTarget = false;
				uint32_t cumSz = 0;
				ASSERT_ONLY(uint32_t eligiblesVisited = 0);
				size_t i = depth, j = 0;
				uint32_t bttop = 0;
				uint32_t btbot = 0;
				uint32_t btham = ham;
				char btchar = 0;
				int btcint = 0;
				uint32_t icur = 0;
				for(; i <= d; i++) {
					if(i < unrevOff) {
						// i is an unrevisitable position, so don't consider it
						continue;
					}
					icur = _qlen - i - 1; // current offset into _qry
					uint8_t qi = QUAL(icur);
					assert_lt(elims[i], 16);
					assert_gt(elims[i], 0);
					if(qi == lowAltQual && elims[i] != 15) {
						// This is an eligible position with at least
						// one remaining backtrack target
						for(j = 0; j < 4; j++) {
							if((elims[i] & (1 << j)) == 0) {
								// This pair has not been eliminated
								assert_gt(PAIR_BOT(i, j), PAIR_TOP(i, j));
								cumSz += PAIR_SPREAD(i, j);
								ASSERT_ONLY(eligiblesVisited++);
								if(r < cumSz) {
									// This is our randomly-selected 
									// backtrack target
									foundTarget = true;
									bttop = PAIR_TOP(i, j);
									btbot = PAIR_BOT(i, j);
									btham += qi;
									btcint = j;
									btchar = "acgt"[j];
									assert_leq(btham, _qualThresh);
									break;
								}
							}
						}
						if(foundTarget) break;
					}
				}
				assert_leq(eligiblesVisited, eligibleNum);
				assert_leq(i, d);
				assert_lt(j, 4);
				assert_neq(0, btchar);
				assert_leq(cumSz, eligibleSz);
				assert(foundTarget);
				assert_gt(btbot, bttop);
				assert_leq(btbot-bttop, eligibleSz);
				// Slide over to the next backtacking frame within
				// pairs and elims; won't interfere with our frame or
				// any of our parents' frames
				uint32_t *newPairs = pairs + (_spread*8);
				uint8_t  *newElims = elims + (_spread);
				// If we've selected a backtracking target that's in
				// the 1-revisitable region, then we ask the recursive
				// callee to consider the 1-revisitable region as also
				// being unrevisitable (since we just "used up" all of
				// our visits)
				uint32_t btUnrevOff  = unrevOff;
				uint32_t btOneRevOff = oneRevOff;
				uint32_t btTwoRevOff = twoRevOff;
				assert_geq(i, unrevOff);
				if(i < oneRevOff) {
					assert_geq(oneRevOff, unrevOff);
					assert_geq(twoRevOff, oneRevOff);
					// Extend unrevisitable region to include former 1-
					// revisitable region
					btUnrevOff = oneRevOff;
					// Extend 1-revisitable region to include former 2-
					// revisitable region
					btOneRevOff = _2revOff;
				}
				else if(i < twoRevOff) {
					assert_geq(oneRevOff, unrevOff);
					assert_geq(twoRevOff, oneRevOff);
					// Extend 1-revisitable region to include former 2-
					// revisitable region; note: this only works
					if(!_halfAndHalf) {
						btOneRevOff = twoRevOff;
					} else {
						// A hacky way of supporting two distinct 1-
						// mismatch areas in the two halves of the seed;
						// the default policy (_halfAndHalf == true)
						// would allow multiple backtracks into the 3'
						// half of the seed
						btTwoRevOff = oneRevOff;
					}
				}
				// Note the character that we're backtracking on in the
				// mm array:
				_mms[stackDepth] = icur;
				_chars[i] = btchar;
				// Now backtrack to target
				ASSERT_ONLY(uint64_t numHits = _params.sink().numHits());
				assert_leq(i+1, _qlen);
				bool ret;
				if(i+1 == _qlen) {
					ret = report(stackDepth+1, bttop, btbot);
				} else {
					ret = backtrack(stackDepth+1,
				                    i+1,
				                    btUnrevOff,  // new unrevisitable boundary
				                    btOneRevOff, // new 1-revisitable boundary
				                    btTwoRevOff, // new 2-revisitable boundary
				                    bttop,  // top arrow in pair prior to 'depth'
				                    btbot,  // bottom arrow in pair prior to 'depth'
				                    btham,  // weighted hamming distance so far
				                    iham,   // initial weighted hamming distance
				                    newPairs,
				                    newElims);
				}
				if(ret) {
					assert_gt(_params.sink().numHits(), numHits);
					if(_os != NULL && (*_os).size() > 0) confirmHit(iham);
					return true; // return, signaling that we've reported
				}
				assert_eq(_params.sink().numHits(), numHits);
				// No hit was reported; update elims[], eligibleSz,
				// eligibleNum, altNum
				_chars[i] = (*_qry)[icur];
				assert_neq(15, elims[i]);
				ASSERT_ONLY(uint8_t oldElim = elims[i]);
				elims[i] |= (1 << j);
				assert_gt(elims[i], oldElim);
				eligibleSz -= (btbot-bttop);
				eligibleNum--;
				assert_geq(eligibleNum, 0);
				altNum--;
				assert_geq(altNum, 0);
				if(altNum == 0) {
					// No alternative backtracking points; all legal
					// backtracking targets have been exhausted
					assert_eq(0, altNum);
					assert_eq(0, eligibleSz);
					assert_eq(0, eligibleNum);
					if(stackDepth == 0) {
						if(_os != NULL && (*_os).size() > 0) confirmNoHit(iham);
					}
					return false;
				}
				else if(eligibleNum == 0) {
					// Find the next set of eligible backtrack points
					// by re-scanning this backtracking frame (from
					// 'depth' up to 'd')
					lowAltQual = 0xff;
					for(size_t k = depth; k <= d; k++) {
						uint32_t kcur = _qlen - k - 1; // current offset into _qry
						uint8_t kq = QUAL(kcur);
						bool kCurIsAlternative = (k >= unrevOff) && (ham + kq <= _qualThresh);
						bool kCurOverridesEligible = false;
						if(kCurIsAlternative) {
							if(kq < lowAltQual) {
								kCurOverridesEligible = true;
							}
							if(kq <= lowAltQual) {
								// Position is eligible
								for(int l = 0; l < 4; l++) {
									if((elims[k] & (1 << l)) == 0) {
										// Not yet eliminated
										if(kCurOverridesEligible) {
											// Clear previous eligible results;
											// this one's better
											lowAltQual = kq;
											kCurOverridesEligible = false;
											eligibleNum = 0;
											eligibleSz = 0;
										}
										eligibleNum++;
										uint32_t spread = PAIR_SPREAD(k, l);
										assert_gt(spread, 0);
										eligibleSz += spread;
									}
								}
							}
						}
					}
				}
				assert_gt(eligibleNum, 0);
				assert_leq(eligibleNum, altNum);
				assert_gt(eligibleSz, 0);
				assert_geq(eligibleSz, eligibleNum);
				assert(sanityCheckEligibility(depth, d, unrevOff, lowAltQual, eligibleSz, eligibleNum, pairs, elims));
				// Try again
			} // while(top == bot && altNum > 0)
			// Mismatch with no alternatives
			if(top == bot && altNum == 0) {
				assert_eq(0, altNum);
				assert_eq(0, eligibleSz);
				assert_eq(0, eligibleNum);
				// Mismatched with no backtracking opportunities;
				// return failure
				if(stackDepth == 0) {
					if(_os != NULL && (*_os).size() > 0) confirmNoHit(iham);
				}
				return false;
			}
			// Match!
			_chars[d] = (*_qry)[cur];
			d++; cur--;
		} // while(cur < _qlen)
		assert_eq(0xffffffff, cur);
		assert_gt(bot, top);
		if(_reportSeedlings > 0) {
			// Stack depth should not exceed given hamming distance
			assert_leq(stackDepth, _reportSeedlings);
		}
		if(stackDepth >= _reportSeedlings) {
			bool ret = report(stackDepth, top, bot);
			if(!ret && stackDepth == 0) {
				if(_os != NULL && (*_os).size() > 0) confirmNoHit(iham);
			}
			if(ret) {
				if(_os != NULL && (*_os).size() > 0) confirmHit(iham);
			}
			return ret;
		} else {
			if(stackDepth == 0) {
				if(_os != NULL && (*_os).size() > 0) confirmNoHit(iham);
			}
			return false;
		}
	}
	
	/**
	 * Print a hit along with information about the backtracking
	 * regions constraining the hit.
	 */
	static void printHit(const vector<TStr>& os,
	                     const Hit& h,
	                     const TStr& qry,
	                     size_t qlen,
	                     uint32_t unrevOff,
	                     uint32_t oneRevOff,
	                     uint32_t twoRevOff,
	                     bool ebwtFw)
	{
		// Print pattern sequence
		cout << "  Pat:  " << qry << endl;
		// Print text sequence
		cout << "  Tseg: ";
		if(ebwtFw) {
			for(size_t i = 0; i < qlen; i++) {
				cout << os[h.h.first][h.h.second + i];
			}
		} else {
			for(int i = (int)qlen-1; i >= 0; i--) {
				cout << os[h.h.first][h.h.second + i];
			}
		}
		cout << endl;
		cout << "  Bt:   ";
		for(int i = (int)qlen-1; i >= 0; i--) {
			if     (i < (int)unrevOff) cout << "0";
			else if(i < (int)oneRevOff)  cout << "1";
			else if(i < (int)twoRevOff)  cout << "2";
			else cout << "X";
		}
		cout << endl;
	}
	/**
	 * Naively search for the same hits that should be found by 
	 */
	static void naiveOracle(const vector<TStr>& os,
	                        const TStr& qry,
	                        uint32_t qlen,
	                        const String<char>& qual,
	                        const String<char>& name,
	                        uint32_t patid,
	                        vector<Hit>& hits,
	                        uint32_t qualThresh,
	                        uint32_t unrevOff,
	                        uint32_t oneRevOff,
	                        uint32_t twoRevOff,
	                        bool fw,
	                        bool ebwtFw,
	                        uint32_t iham = 0,
	                        String<QueryMutation>* muts = NULL,
	                        bool halfAndHalf = false)
	{
		typedef typename Value<TStr>::Type TVal;
		bool fivePrimeOnLeft = (ebwtFw == fw);
	    uint32_t plen = qlen;
		uint8_t *pstr = (uint8_t *)begin(qry, Standard());
	    // For each text...
		for(size_t i = 0; i < os.size(); i++) {
			// For each text position...
			if(length(os[i]) < plen) continue;
			uint32_t olen = length(os[i]);
			uint8_t *ostr = (uint8_t *)begin(os[i], Standard());
			// For each possible alignment of pattern against text
			for(size_t j = 0; j <= olen - plen; j++) {
				size_t rev1mm  = 0; // mismatches observed in the 1-revisitable region
				size_t rev2mm  = 0; // mismatches observed in the 2-revisitable region
				uint32_t ham = iham; // weighted hamming distance so far
				bitset<max_read_bp> diffs = 0; // mismatch bitvector
				// For each alignment column, from right to left
				bool success = true;
				int ok, okInc;
				if(ebwtFw) {
					ok = j+(int)plen-1;
					okInc = -1;
				} else {
					ok = olen-(j+((int)plen-1))-1;
					okInc = 1;
				}
				for(int k = (int)plen-1; k >= 0; k--) {
					size_t kr = plen-1-k;
					if(pstr[k] != ostr[ok]) {
						ham += QUAL2(qual, k);
						if(ham > qualThresh) {
							// Alignment is invalid because it exceeds
							// our target weighted hamming distance
							// threshold
							success = false;
							break;
						}
						// What region does the mm fall into?
						if(kr < unrevOff) {
							// Alignment is invalid because it contains
							// a mismatch in the unrevisitable region
							success = false;
							break;
						} else if(kr < oneRevOff) {
							rev1mm++;
							if(rev1mm > 1 && !halfAndHalf) {
								// Alignment is invalid because it
								// contains more than 1 mismatch in the
								// 1-revisitable region
								success = false;
								break;
							}
						} else if(kr < twoRevOff) {
							rev2mm++;
							if(rev2mm > 2 && !halfAndHalf) {
								// Alignment is invalid because it
								// contains more than 2 mismatches in the
								// 2-revisitable region
								success = false;
								break;
							}
						}
						if(halfAndHalf && (rev1mm > 1 || rev2mm > 1)) {
							// Half-and-half alignment is invalid
							// because it contains more than 1 mismatch
							// in either one or the other half
							success = false;
							break;
						}
						if(fivePrimeOnLeft) {
							diffs.set(k);
						} else {
							// The 3' end is on on the left end of the
							// pattern, but the diffs vector should
							// encode mismatches w/r/t the 5' end, so
							// we flip
							diffs.set(plen-k-1);
						}
					}
					ok += okInc;
				}
				if(halfAndHalf && success && (rev1mm != 1 || rev2mm != 1)) {
					success = false;
				}
				if(success) {
					// It's a hit
					uint32_t off = j;
					if(!ebwtFw) {
						off = olen - off;
						off -= plen;
					}
					// Add in mismatches from _muts
					if(muts != NULL) {
						for(size_t i = 0; i < length(*muts); i++) {
							// Entries in _mms[] are in terms of offset into
							// _qry - not in terms of offset from 3' or 5' end
							if(fivePrimeOnLeft) {
								diffs.set((*muts)[i].pos);
							} else {
								diffs.set(plen - (*muts)[i].pos - 1);
							}
						}
					}
					Hit h(make_pair(i, off), 
						  patid,  // read id
						  name,   // read name
						  qry,    // read sequence
						  qual,   // read qualities 
						  fw,     // forward/reverse-comp
						  diffs); // mismatch bitvector
					hits.push_back(h);
				} // For each pattern character
			} // For each alignment over current text
		} // For each text
	}


protected:
	
	void applyMutations() {
		if(_muts == NULL) {
			// No mutations to apply
			return;
		}
		for(size_t i = 0; i < length(*_muts); i++) {
			const QueryMutation& m = (*_muts)[i];
			assert_lt(m.pos, _qlen);
			assert_lt(m.oldBase, 4);
			assert_lt(m.newBase, 4);
			assert_neq(m.oldBase, m.newBase);
			assert_eq((uint32_t)((*_qry)[m.pos]), (uint32_t)m.oldBase);
			(*_qry)[m.pos] = (Dna)(int)m.newBase; // apply it
		}
	}
	
	void undoMutations() {
		if(_muts == NULL) {
			// No mutations to undo
			return;
		}
		for(size_t i = 0; i < length(*_muts); i++) {
			const QueryMutation& m = (*_muts)[i];
			assert_lt(m.pos, _qlen);
			assert_lt(m.oldBase, 4);
			assert_lt(m.newBase, 4);
			assert_neq(m.oldBase, m.newBase);
			assert_eq((uint32_t)((*_qry)[m.pos]), (uint32_t)m.newBase);
			(*_qry)[m.pos] = (Dna)(int)m.oldBase; // undo it
		}
	}
	
	bool report(uint32_t stackDepth, uint32_t top, uint32_t bot) {
		if(_reportSeedlings) {
			assert_leq(stackDepth, _reportSeedlings);
			reportSeedling(stackDepth);
			return false; // keep going
		} else {
			// Undo all the mutations
			ASSERT_ONLY(TStr tmp = (*_qry));
			undoMutations();
			bool hit;
			if(_muts != NULL) {
				assert_neq(tmp, (*_qry));
				size_t numMuts = length(*_muts);
				for(size_t i = 0; i < numMuts; i++) {
					// Entries in _mms[] are in terms of offset into
					// _qry - not in terms of offset from 3' or 5' end
					_mms[stackDepth + i] = (*_muts)[i].pos;
				}
				hit = reportHit(stackDepth+numMuts, top, bot);
			} else {
				hit = reportHit(stackDepth, top, bot);
			}
			applyMutations();
			assert_eq(tmp, (*_qry));
			return hit;
		}
	}

	/**
	 * Report a hit with # mismatches = stackDepth, at rows delimited
	 * by top and bot
	 */
	bool reportHit(uint32_t stackDepth, uint32_t top, uint32_t bot) {
		// Possibly report
		if(_oneHit) {
			uint32_t spread = bot - top;
			uint32_t r = top + (_rand.nextU32() % spread);
			for(uint32_t i = 0; i < spread; i++) {
				uint32_t ri = r + i;
				if(ri >= bot) ri -= spread;
				// reportChaseOne takes ths _mms[] list in terms of
				// their indices into the query string; not in terms
				// of their offset from the 3' or 5' end.
				if(_ebwt.reportChaseOne((*_qry), _qual, _name,
				                        _mms, stackDepth, ri,
				                        top, bot, _qlen, _params))
				{
					return true;
				}
			}
			return false;
		} else {
			// Not yet smart enough to report all hits
			assert(false);
			return false;
		}
	}

	/**
	 * Report a "seedling hit" - i.e. report the mismatches that got us
	 * here.
	 */
	bool reportSeedling(uint32_t stackDepth) {
		// Possibly report
		assert_gt(_reportSeedlings, 0);
		assert(_seedlings != NULL);
		for(size_t i = 0; i < stackDepth; i++) {
			// Enrties in _mms[] hold the offset from the 5' end 
			assert_lt(_mms[i], _qlen);
			append((*_seedlings), (uint8_t)_mms[i]); // pos
			uint32_t ci = _qlen - _mms[i] - 1;
			// _chars[] is index in terms of RHS-relative depth
			int c = (int)(Dna)_chars[ci];
			assert_lt(c, 4);
			assert_neq(c, (int)(*_qry)[_mms[i]]);
			append((*_seedlings), (uint8_t)c); // chr
			if(i < stackDepth - 1) {
				append((*_seedlings), 0xfe); // minor separator
			}
		}
		return true;
	}

	/**
	 * Check that the given eligibility parameters (lowAltQual,
	 * eligibleSz, eligibleNum) are correct, given the appropriate
	 * inputs (pairs, elims, depth, d, unrevOff)
	 */
	bool sanityCheckEligibility(uint32_t  depth,
	                            uint32_t  d,
	                            uint32_t  unrevOff,
	                            uint32_t  lowAltQual,
	                            uint32_t  eligibleSz,
	                            uint32_t  eligibleNum,
	                            uint32_t* pairs,
	                            uint8_t*  elims)
	{
		// Sanity check that the lay of the land is as we
		// expect given eligibleNum and eligibleSz
		size_t i = max(depth, unrevOff), j = 0;
		uint32_t cumSz = 0;
		uint32_t eligiblesVisited = 0;
		for(; i <= d; i++) {
			uint32_t icur = _qlen - i - 1; // current offset into _qry
			uint8_t qi = QUAL(icur);
			assert_lt(elims[i], 16);
			assert_gt(elims[i], 0);
			if(qi == lowAltQual && elims[i] != 15) {
				// This is an eligible position with at least
				// one remaining backtrack target
				for(j = 0; j < 4; j++) {
					if((elims[i] & (1 << j)) == 0) {
						// This pair has not been eliminated
						assert_gt(PAIR_BOT(i, j), PAIR_TOP(i, j));
						cumSz += PAIR_SPREAD(i, j);
						eligiblesVisited++;
					}
				}
			}
		}
		assert_eq(cumSz, eligibleSz);
		assert_eq(eligiblesVisited, eligibleNum);
		return true;
	}
	
	/**
	 * Confirm that no 
	 */
	void confirmNoHit(uint32_t iham) {
		// Not smart enough to deal with seedling hits yet
		if(_os == NULL || (*_os).size() == 0 || _reportSeedlings > 0) return;
		vector<Hit> oracleHits;
		// Invoke the naive oracle, which will place all qualifying
		// hits in the 'oracleHits' vector
		naiveOracle(oracleHits, iham);
		if(oracleHits.size() > 0) {
			// Oops, the oracle found at least one hit; print
			// detailed info about the first oracle hit (for
			// debugging)
			const Hit& h = oracleHits[0];
			cout << "Oracle hit " << oracleHits.size()
			     << " times, but backtracker did not hit" << endl;
			cout << "First oracle hit: " << endl;
			if(_muts != NULL) {
				undoMutations();
				cout << "  Unmutated Pat:  " << prefix(*_qry, _qlen) << endl;
				applyMutations();
			}
			cout << "  Pat:            " << prefix(*_qry, _qlen) << endl;
			cout << "  Tseg:           ";
			bool ebwtFw = _params.ebwtFw();
			if(ebwtFw) {
				for(size_t i = 0; i < _qlen; i++) {
					cout << (*_os)[h.h.first][h.h.second + i];
				}
			} else {
				for(int i = (int)_qlen-1; i >= 0; i--) {
					cout << (*_os)[h.h.first][h.h.second + i];
				}
			}
			cout << endl;
			cout << "  Quals:          " << prefix(*_qual, _qlen) << endl;
			cout << "  Bt:             ";
			for(int i = (int)_qlen-1; i >= 0; i--) {
				if     (i < (int)_unrevOff) cout << "0";
				else if(i < (int)_1revOff)  cout << "1";
				else if(i < (int)_2revOff)  cout << "2";
				else cout << "X";
			}
			cout << endl;
		}
		assert_eq(0, oracleHits.size());
	}
	
	/**
	 * 
	 */
	void confirmHit(uint32_t iham) {
		// Not smart enough to deal with seedling hits yet
		if(_os == NULL || (*_os).size() == 0 || _reportSeedlings > 0) return;
		vector<Hit> oracleHits;
		// Invoke the naive oracle, which will place all qualifying
		// hits in the 'oracleHits' vector
		naiveOracle(oracleHits, iham);
		vector<Hit>& retainedHits = _params.sink().retainedHits();
		// If we found a hit, it had better be one of the ones
		// that the oracle found
		assert_gt(oracleHits.size(), 0);
		// Get the hit reported by the backtracker
		Hit& rhit = retainedHits.back();
		// Go through oracleHits and look for a match
		size_t i;
		for(i = 0; i < oracleHits.size(); i++) {
			const Hit& h = oracleHits[i];
    		if(h.h.first == rhit.h.first && h.h.second == rhit.h.second) {
    			assert_eq(h.fw, rhit.fw);
    			assert_eq(h.mms, rhit.mms);
    			// It's a match - hit confirmed
    			break;
    		}
		}
		assert_lt(i, oracleHits.size()); // assert we found a matchup
	}
	
	/**
	 * Naively search for hits for the current pattern under the
	 * current backtracking strategy and store hits in hits vector.
	 */
	void naiveOracle(vector<Hit>& hits,
	                 uint32_t iham = 0, /// initial weighted hamming distance
	                 uint32_t unrevOff = 0xffffffff,
	                 uint32_t oneRevOff = 0xffffffff,
	                 uint32_t twoRevOff = 0xffffffff)
	{
		typedef typename Value<TStr>::Type TVal;
		if(unrevOff  == 0xffffffff) unrevOff  = _unrevOff;
		if(oneRevOff == 0xffffffff) oneRevOff = _1revOff;
		if(twoRevOff == 0xffffffff) twoRevOff = _2revOff;
		bool ebwtFw = _params.ebwtFw();
		bool fw = _params.fw();
		uint32_t patid = _params.patId();
		
		naiveOracle((*_os),
		            (*_qry),
		            _qlen,
		            (*_qual),
		            (*_name),
		            patid,
		            hits,
		            _qualThresh,
		            unrevOff,
		            oneRevOff,
		            twoRevOff,
		            fw,    // transpose
		            ebwtFw,
		            iham,
		            _muts,
		            _halfAndHalf);
	}
	
	TStr*               _qry;    // query (read) sequence
	size_t              _qlen;   // length of _qry
	String<char>*       _qual;   // quality values for _qry
	String<char>*       _name;   // name of _qry
	const Ebwt<TStr>&   _ebwt;   // Ebwt to search in
	const EbwtSearchParams<TStr>& _params;   // Ebwt to search in
	String<uint8_t>*    _btEquivs; // backtracking equivalence classes
	uint32_t            _unrevOff; // unrevisitable chunk
	uint32_t            _1revOff;  // 1-revisitable chunk
	uint32_t            _2revOff;  // 2-revisitable chunk
	uint32_t            _itop;   // initial top arrow, or 0xffffffff if
	                             // we're starting from the beginning
	                             // of the query
	uint32_t            _ibot;   // initial bot arrow, or 0xffffffff if
	                             // we're starting from the beginning
	                             // of the query
	uint32_t            _spread; // size of window within which to
	                             // backtrack
	uint32_t            _maxStackDepth;
	uint32_t            _qualThresh; // only accept hits with weighted
	                             // hamming distance <= _qualThresh
	uint32_t            _qualWobble; // hits within _qualWobble weighted
	                             // hamming distance of each other are
	                             // considered equally good
	bool                _oneHit; // stop backtracking after finding 1
	                             // legit hit?  (doesn't really work -
	                             // we always operate in _oneHit mode)
	bool                _reportOnHit; // report as soon as we find a
	                             // hit? (as opposed to leaving it up
	                             // to the caller whether to report)
	uint32_t           *_pairs;  // arrow pairs, leveled in parallel
	                             // with decision stack
	uint8_t            *_elims;  // which arrow pairs have been
	                             // eliminated, leveled in parallel
	                             // with decision stack
	uint8_t            *_bts;    // how many backtracks remain in each
	                             // equivalence class
	uint32_t           *_mms;    // array for holding mismatches
	// Entries in _mms[] are in terms of offset into
	// _qry - not in terms of offset from 3' or 5' end
	char               *_chars;  // characters selected so far
	uint32_t            _reportSeedlings; // if > 0, report seedling
	                             // hits up to this many mismatches
	String<uint8_t>    *_seedlings; // append seedling hits here
	String<QueryMutation> *_muts;// set of mutations that apply for a
	                             // seedling
	vector<TStr>*       _os;     // reference texts
	bool                _halfAndHalf;
	uint32_t            _5depth; // depth of 5'-seed-half border
	uint32_t            _3depth; // depth of 3'-seed-half border
	String<char>        _nameDefault; // default name, for when it's
	                             // not specified by caller
	String<char>        _qualDefault; // default quals
	RandomSource        _rand;   // Source of pseudo-random numbers
	bool                _verbose;// be talkative
};

#endif /*EBWT_SEARCH_BACKTRACK_H_*/