/*
taxator-tk predicts the taxon for DNA sequences based on sequence alignment.

Copyright (C) 2010 Johannes Dröge

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef taxonpredictionmodelsequence_hh_
#define taxonpredictionmodelsequence_hh_

#include <seqan/align.h>
#include <seqan/graph_msa.h>
#include <seqan/file.h>
#include <seqan/basic.h>
#include <boost/tuple/tuple.hpp>
#include <boost/tuple/tuple_comparison.hpp>
#include <boost/format.hpp>
#include <assert.h>
#include <limits>
#include <set>
#include <ostream>
#include <cmath>
#include "taxonpredictionmodel.hh"
#include "sequencestorage.hh"
#include "profiling.hh"

// helper class
class BandFactor {
public:
    BandFactor(TaxonomyInterface& taxinter, uint reserve = 0) :
        bandfactor_(-1),
        taxinter_(taxinter) {
        if(reserve) data_.reserve(reserve);
    }

    void addSequence(const int score, const TaxonNode* node) {
        data_.push_back(boost::make_tuple(score, node));
    }

    float getFactor() {
        if(bandfactor_ < 0) {
            sort();
            setBandFactor();
        }
        return bandfactor_;
    }

private:
    void setBandFactor(const float min_bandfactor = 1., const float max_bandfactor = std::numeric_limits<  int >::max()) { //data_ must be sorted TODO: optimize
        float bandfactor = min_bandfactor;
        int score;
        const TaxonNode* anchor;
        const TaxonNode* node;
        std::map< small_unsigned_int, int > worstscore_per_rank;
        boost::tie(score, anchor) = data_[0];
        small_unsigned_int last_rank = anchor->data->root_pathlength;
        worstscore_per_rank[ last_rank ] = score; //TODO: change name

        for (uint i = 1; i < data_.size(); ++i) {
            boost::tie(score, node) = data_[i];
            node = taxinter_.getLCA(node, anchor);
            small_unsigned_int rank = node->data->root_pathlength;
            if (rank == last_rank) {
            } else if (rank < last_rank) {
                worstscore_per_rank[rank] = score;
                last_rank = rank;
            } else { //disorder
                int refscore;
                small_unsigned_int r = rank - 1;
                do {
                    std::map< small_unsigned_int, int >::iterator it = worstscore_per_rank.find(r);
                    if (it != worstscore_per_rank.end()) {
                        refscore = it->second;
                        if (refscore) bandfactor = std::max(bandfactor, float(score)/float(refscore));
                    }
                } while (r--);
            }
        }
        bandfactor_ = std::min(bandfactor, max_bandfactor);
    }

    void sort() { //sort by increasing order
        data_type_::iterator start_it = data_.begin();
        std::sort(++start_it, data_.end(), comparator_);
    }

    compareTupleFirstLT< boost::tuple< int, const TaxonNode* >, 0 > comparator_;
    typedef std::vector< boost::tuple< int, const TaxonNode* > > data_type_;
    float bandfactor_;
    data_type_ data_;
    TaxonomyInterface taxinter_;

};

// TODO: make timers thread-safe
template< typename ContainerT, typename QStorType, typename DBStorType >
class RPAPredictionModel : public TaxonPredictionModel< ContainerT > {
public:
    RPAPredictionModel(const Taxonomy* tax, QStorType& q_storage, const DBStorType& db_storage, float exclude_factor ,float reeval_bandwidth = .1) :
        TaxonPredictionModel< ContainerT >(tax),
        query_sequences_(q_storage),
        db_sequences_(db_storage),
        exclude_alignments_factor_(exclude_factor),
        reeval_bandwidth_factor_(1. - reeval_bandwidth), //TODO: check range
        measure_sequence_retrieval_("sequence retrieval using index"),
        measure_pass_0_alignment_("best reference re-evaluation alignments (pass 0)"),
        measure_pass_1_alignment_("best reference anchor alignments (pass 1)"),
        measure_pass_2_alignment_("distant anchor alignments (pass 2)")
    {};

    void predict(ContainerT& recordset, PredictionRecord& prec, std::ostream& logsink) {
        this->initPredictionRecord(recordset, prec);  // just set query name and length
        const std::string& qid = prec.getQueryIdentifier();

        // cannot be part of class because it is not thread-safe
        boost::format seqname_fmt("%d:%d@%s");

        // determine best local alignment score and number of unmasked records
        uint n = 0;  // number of alignments/reference segments
        uint n_pre = 0;
        float qmaxscore = .0;
        for(typename ContainerT::iterator rec_it = recordset.begin(); rec_it != recordset.end(); ++rec_it) {
            if(!(*rec_it)->isFiltered()) {
                qmaxscore = std::max(qmaxscore, (*rec_it)->getScore());
                ++n;
            }
        }

        // TODO: push records to temporary list
        // TODO: reduce number of records based on simple PID heuristic
        {
            n_pre = n;
            const float threshold = qmaxscore*exclude_alignments_factor_;
            for(typename ContainerT::iterator rec_it = recordset.begin(); rec_it != recordset.end(); ++rec_it) {
                if(!(*rec_it)->isFiltered() && (*rec_it)->getScore() < threshold) {
                    (*rec_it)->filterOut();
                    --n;
                }
            }
        }
        
        // with no unmasked alignment, set to unclassified and return
        if(n==0) {  //TODO: record should not be reported at all in GFF3
            logsink << "ID\t" << boost::str(seqname_fmt % -1 % -1 % qid) << std::endl;
            logsink << "  NUMREF\t" << n_pre << tab << n << std::endl << std::endl;
            
            TaxonPredictionModel< ContainerT >::setUnclassified(prec);
            return;
        }
                
        // with one alignment, don't align and return
        if(n==1) {
            typename ContainerT::iterator rec_it = firstUnmaskedIter(recordset);
            large_unsigned_int qrstart = (*rec_it)->getQueryStart();
            large_unsigned_int qrstop = (*rec_it)->getQueryStop();
            
            logsink << "ID\t" << boost::str(seqname_fmt % qrstart % qrstop % qid) << std::endl;
            logsink << "  NUMREF\t" << n_pre << tab << n << std::endl << std::endl;
            
            prec.setQueryFeatureBegin(qrstart);
            prec.setQueryFeatureEnd(qrstop);
            prec.setInterpolationValue(1.);
            prec.setNodeRange((*rec_it)->getReferenceNode(), this->taxinter_.getRoot(), (*rec_it)->getIdentities());
            prec.setBestReferenceTaxon((*rec_it)->getReferenceNode());
            return;
        }
        
        // n>1 -> screen alignments and determine query range
        large_unsigned_int qrstart;
        large_unsigned_int qrstop;
        {
            typename ContainerT::iterator rec_it = firstUnmaskedIter(recordset);
            qrstart = (*rec_it)->getQueryStart();
            qrstop = (*rec_it)->getQueryStop();
            while (++rec_it != recordset.end()) {
                if (! (*rec_it)->isFiltered()) {
                    qrstart = std::min((*rec_it)->getQueryStart(), qrstart);
                    qrstop = std::max((*rec_it)->getQueryStop(), qrstop);
                }
            }
        }
        const large_unsigned_int qrlength = qrstop - qrstart + 1;
        
        // logging
        const std::string qrseqname = boost::str(seqname_fmt % qrstart % qrstop % qid);
        logsink << "ID\t" << qrseqname << std::endl;
        logsink << "  NUMREF\t" << n_pre << tab << n << std::endl << std::endl;
        
        // count number of alignment calculations in each of the three passes
        uint gcounter = 0;
        uint pass_0_counter = 0;
        uint pass_1_counter = 0;
        uint pass_2_counter = 0;
        
        // data storage  TODO: use of C-style arrays
        const seqan::Dna5String qrseq = query_sequences_.getSequence(qid, qrstart, qrstop);
        std::vector< AlignmentRecordTaxonomy* > records_ordered;
        records_ordered.reserve(n);
        std::vector< seqan::Dna5String > rrseqs_ordered; //TODO: boost ptr_container/seqan::StringSet/set to detect equal sequences
        rrseqs_ordered.reserve(n);
//         std::vector< std::string > rrseqs_ordered_names;
        std::vector< int > rrseqs_qscores;
        rrseqs_qscores.reserve(n);
        std::vector< large_unsigned_int > rrseqs_matches;
        rrseqs_matches.reserve(n);
        
        {   // retrieve segment sequences
            measure_sequence_retrieval_.start();
            typename ContainerT::iterator rec_it = recordset.begin();
            while (rec_it != recordset.end()) {
                if (! (*rec_it)->isFiltered()) {
                    records_ordered.push_back(*rec_it);
                    AlignmentRecordTaxonomy& rec = **rec_it;
                    const std::string& rid = rec.getReferenceIdentifier();
                    large_unsigned_int rstart = rec.getReferenceStart();
                    large_unsigned_int rstop = rec.getReferenceStop();
                    large_unsigned_int left_ext = rec.getQueryStart() - qrstart;
                    large_unsigned_int right_ext = qrstop - rec.getQueryStop();

                    // cut out reference region
                    if(rstart <= rstop) {
                        large_unsigned_int start = left_ext < rstart ? rstart - left_ext : 1;
                        large_unsigned_int stop = rstop + right_ext;
                        rrseqs_ordered.push_back(db_sequences_.getSequence(rid, start, stop)); //TODO: avoid copying
//                         rrseqs_ordered_names.push_back(boost::str(seqname_fmt % start % stop % rid));
                    } else {
                        large_unsigned_int start = right_ext < rstop ? rstop - right_ext : 1;
                        large_unsigned_int stop = rstart + left_ext;
                        rrseqs_ordered.push_back(db_sequences_.getSequenceReverseComplement(rid, start, stop)); //TODO: avoid copying
//                         rrseqs_ordered_names.push_back(boost::str(seqname_fmt % stop % start % rid));
                    }
                }
                ++rec_it;
            }
            measure_sequence_retrieval_.stop();
        }

        std::set<uint> qgroup;
        large_unsigned_int anchors_support = 0;
        const TaxonNode* rtax = NULL;  // taxon of closest evolutionary neighbor(s)
        const TaxonNode* lca_allnodes = records_ordered.front()->getReferenceNode();  // used for optimization
        
        {   // pass 0 (re-alignment to most similar reference segments)
            logsink << "  PASS\t0" << std::endl;
            measure_pass_0_alignment_.start();
            float dbalignment_score_threshold = reeval_bandwidth_factor_*qmaxscore;
            uint index_best = 0;

            for (uint i = 0; i < n; ++i) { //calculate scores for best-scoring references
                int score;
                large_unsigned_int matches;
                
                if(records_ordered[i]->getAlignmentLength() == qrlength && records_ordered[i]->getIdentities() == qrlength) {
                    qgroup.insert(i);
                    score = 0;
                    matches = records_ordered[i]->getIdentities();
                    logsink << "    *ALN " << i << " <=> query\tscore = " << score << "; matches = " << matches << std::endl;
                } else if (records_ordered[i]->getScore() >= dbalignment_score_threshold) {
                    qgroup.insert(i);
                    score = -seqan::globalAlignmentScore(rrseqs_ordered[i], qrseq, seqan::MyersBitVector());
                    ++pass_0_counter;
                    matches = std::max(static_cast<large_unsigned_int>(std::max(seqan::length(rrseqs_ordered[i]), seqan::length(qrseq)) - score), records_ordered[i]->getIdentities());
                    logsink << "    +ALN " << i << " <=> query\tscore = " << score << "; matches = " << matches << std::endl;
                } else {  // not similar -> fill in some dummy values
                    score = std::numeric_limits< int >::max();
                    matches = 0;
                }
                rrseqs_qscores.push_back(score);
                rrseqs_matches.push_back(matches);
                
                if (score < rrseqs_qscores[index_best] || (score == rrseqs_qscores[index_best] && matches > rrseqs_matches[index_best])) {
                    index_best = i;
                }
                
                anchors_support = std::max(anchors_support, matches);  //TODO: move to previous if-statement?
                
                lca_allnodes = this->taxinter_.getLCA(lca_allnodes, records_ordered[i]->getReferenceNode());
            }

            // only keep and use the best-scoring reference sequences
//             std::list<const TaxonNode*> rtaxa_list;
            rtax = records_ordered[index_best]->getReferenceNode();
            for (std::set< uint >::iterator it = qgroup.begin(); it != qgroup.end();) {
                if (rrseqs_qscores[*it] != rrseqs_qscores[index_best] || rrseqs_matches[*it] != rrseqs_matches[index_best]) qgroup.erase(it++);
                else {
                    const TaxonNode* cnode = records_ordered[*it]->getReferenceNode();
                    rtax = this->taxinter_.getLCA(rtax, cnode);
                    logsink << "      current ref node: " << "("<< rrseqs_qscores[*it] <<") "<< rtax->data->annotation->name << " (+ " << cnode->data->annotation->name << " )" << std::endl;
                    ++it;
                }
            }
            
            assert(! qgroup.empty());/* {  //debug code
                std::cerr << "score = " << rrseqs_qscores[index_best] << "; matches = " << rrseqs_matches[index_best] << std::endl;
                exit(1);
            };*/
            
            measure_pass_0_alignment_.stop();
            logsink << "    NUMALN\t" << pass_0_counter << std::endl << std::endl;
        }

        std::vector< const TaxonNode* > anchors_lnode;      // taxa closer to best ref than query
        std::vector< const TaxonNode* > anchors_unode;      // outgroup-related taxa
        float anchors_taxsig = 1.;                          // a measure of tree-like scores  
        float ival_global = 0.;
        const TaxonNode* lnode_global = rtax;
        const TaxonNode* unode_global = rtax;
        std::set<uint> outgroup;                 // outgroup sequences
        float bandfactor_max = 1.;

        {   // pass 1 (best reference alignment)
            measure_pass_1_alignment_.start();
            logsink << "  PASS\t1" << std::endl;
            const std::size_t num_qnodes = qgroup.size();
            anchors_lnode.reserve(num_qnodes);  //TODO: performance check
            anchors_unode.reserve(num_qnodes + 1);  // minimum

            uint alignments_counter = 0;
            uint alignments_counter_naive = 0;
            small_unsigned_int lca_root_dist_min = std::numeric_limits<small_unsigned_int>::max();
            do {  // determine query taxon range
                BandFactor bandfactor1(this->taxinter_, n);
                const uint index_anchor = *qgroup.begin();
                qgroup.erase(qgroup.begin());
                const int qscore = rrseqs_qscores[index_anchor];
                const TaxonNode* rnode = records_ordered[index_anchor]->getReferenceNode();
                bandfactor1.addSequence(0, rnode);
                const TaxonNode* lnode = rtax;
                const TaxonNode* unode = NULL;
                int lscore = 0;
                int uscore = std::numeric_limits<int>::max();

                std::list< boost::tuple< uint, int > > outgroup_tmp;

                // align all others <=> anchor TODO: adaptive cut-off
                logsink << "      query: (" << qscore << ") unknown" << std::endl;
                alignments_counter_naive += n - 1;
                for(uint i = n; lnode != this->taxinter_.getRoot() && i-- > 0;) {  // reverse order saves some alignments
                    const TaxonNode* cnode = records_ordered[i]->getReferenceNode();
                    int score, matches;
                    if (i == index_anchor) score = 0;
                    else {
                        // use triangle relation to avoid alignment
                        if (rrseqs_qscores[i] == 0 && rrseqs_qscores[index_anchor] == 0 ) { //&& rrseqs_matches[i]) {
                            score = rrseqs_qscores[i];
                            matches = rrseqs_matches[i];
                        }
                        else {
                            score = -seqan::globalAlignmentScore(rrseqs_ordered[ i ], rrseqs_ordered[ index_anchor ], seqan::MyersBitVector());
                            ++pass_1_counter;
                            ++alignments_counter;
                            matches = std::max(seqan::length(rrseqs_ordered[ i ]), seqan::length(rrseqs_ordered[ index_anchor ])) - score;
                            logsink << "    +ALN " << i << " <=> " << index_anchor << "\tscore = " << score << "; matches = " << matches << std::endl;
                            
                            // update query alignment scores using triangle relation
                            if (rrseqs_qscores[index_anchor] == 0 && rrseqs_matches[i]) {  // TODO: why rrseqs_matches[i]? 
                                rrseqs_qscores[i] = score;
                                rrseqs_matches[i] = matches;
                            }
                        }
                    }
                    
                    bandfactor1.addSequence(score, cnode);

                    // place sequence
                    if (score == 0) qgroup.erase(i);  // remove this from list of qnodes because it is sequence-identical
                    else {
                        if(score <= qscore) {
                            lnode = this->taxinter_.getLCA(lnode, cnode);
                            if(score > lscore) lscore = score;
                            logsink << "      current lower node: " << "("<< score <<") "<<lnode->data->annotation->name << " (+ " << cnode->data->annotation->name << " at " << static_cast<int>(this->taxinter_.getLCA(cnode, rnode)->data->root_pathlength) << " )" << std::endl;
//                             if(lnode == this->taxinter_.getRoot()) {
//                                 unode = this->taxinter_.getRoot();
//                                 break;
//                             }
                        }
                        else {
                            if(score < uscore) uscore = score;
                            outgroup_tmp.push_back(boost::make_tuple(i,score));
                        }
//                             // TODO: change logic in outgroup; keep highest taxon with worst score
//                             if(score == min_upper_score) {
//                                 unode = this->taxinter_.getLCA(cnode, this->taxinter_.getLCA(lnode, unode));
//                                 logsink << "      current upper node: " << "("<< score <<") "<< unode->data->annotation->name << " (+ " << cnode->data->annotation->name << " at " << static_cast<int>(this->taxinter_.getLCA(cnode, rnode)->data->root_pathlength) << " )" << std::endl;
//                             }
//                             else if (score < min_upper_score) {
//                                 uscore = score;
//                                 min_upper_score = score;
//                                 unode = this->taxinter_.getLCA(cnode, lnode);
//                                 outgroup_tmp.clear();
//                                 logsink << "      current upper node: " << "("<< score <<") "<< unode->data->annotation->name << " (* " << cnode->data->annotation->name << " at " << static_cast<int>(this->taxinter_.getLCA(cnode, rnode)->data->root_pathlength) << " )" << std::endl;
//                             }
//                         }
                    }
                }

                float bandfactor = bandfactor1.getFactor();
                bandfactor_max = std::max(bandfactor_max, bandfactor);
                int qscore_ex = qscore * bandfactor;
                int min_upper_score = std::numeric_limits< int >::max();
                
                logsink << std::endl << "    EXT\tqscore = " << qscore << "; threshold = " << qscore_ex << "; bandfactor = " << bandfactor << std::endl;
                for(std::list< boost::tuple<uint,int> >::iterator it = outgroup_tmp.begin(); it != outgroup_tmp.end();) {
//                     uint i;
                    int score = it->get<1>();
//                     boost::tie(i, score) = *it;
//                     const TaxonNode* cnode = records_ordered[i]->getReferenceNode();
//                     if(score <= qscore_ex) {
//                         unode = this->taxinter_.getLCA(unode, cnode);
//                         min_upper_score = std::min(min_upper_score, score);
//                         if(score > lscore) lscore = score;
//                         logsink << "      current upper node: " << "("<< score <<") "<<unode->data->annotation->name << " (+ " << cnode->data->annotation->name << " at " << static_cast<int>(this->taxinter_.getLCA(cnode, rnode)->data->root_pathlength) << " )" << std::endl;
//                         ++it;
//                     }
//                     else {
                    if(score > qscore_ex) {
                        if (score > min_upper_score) it = outgroup_tmp.erase(it);
                        else {
                            if(score < min_upper_score) min_upper_score = score;
                            ++it;
                        }
                    } else {
                        if(min_upper_score > qscore_ex) min_upper_score = score;
                        else min_upper_score = std::max(min_upper_score, score);
                        ++it;
                    }
//                     else {
//                         if(score == min_upper_score) {
//                             unode = this->taxinter_.getLCA(cnode, unode);
//                             logsink << "      current upper node: " << "("<< score <<") "<< unode->data->annotation->name << " (+ " << cnode->data->annotation->name << " at " << static_cast<int>(this->taxinter_.getLCA(cnode, rnode)->data->root_pathlength) << " )" << std::endl;
//                         }
//                         else {  // if (score > min_upper_score)
//                             min_upper_score = uscore = score;
//                             unode = this->taxinter_.getLCA(cnode, lnode);
//                             logsink << "      current upper node: " << "("<< score <<") "<< unode->data->annotation->name << " (* " << cnode->data->annotation->name << " at " << static_cast<int>(this->taxinter_.getLCA(cnode, rnode)->data->root_pathlength) << " )" << std::endl;
//                         }
//                         ++it;
//                     }
//                     }
                }
                
                // push elements from temporary to outgroup set
                if(min_upper_score != std::numeric_limits< int >::max()) unode = lnode;
                for(std::list< boost::tuple<uint,int> >::iterator it = outgroup_tmp.begin(); it != outgroup_tmp.end(); ++it) {
                    uint i;
                    int score;
                    boost::tie(i, score) = *it;
                    const TaxonNode* cnode = records_ordered[i]->getReferenceNode();
                    
                    if(score > min_upper_score) continue;
                    
                    // add to upper node if(score <= min_upper_score)
                    unode = this->taxinter_.getLCA(cnode, unode);
                    logsink << "      current upper node: " << "("<< score <<") "<< unode->data->annotation->name << " (+ " << cnode->data->annotation->name << " at " << static_cast<int>(this->taxinter_.getLCA(cnode, rnode)->data->root_pathlength) << " )" << std::endl;

                    // curate minimal outgroup TODO: only keep score == min_upper_score in outgroup?
                    const small_unsigned_int lca_root_dist = this->taxinter_.getLCA(cnode, rtax)->data->root_pathlength;
                    if(lca_root_dist > lca_root_dist_min) continue;
                    else if(lca_root_dist < lca_root_dist_min) {
                        lca_root_dist_min = lca_root_dist;
                        outgroup.clear();
                    }
                    outgroup.insert(i);
                }

                // adjust interpolation value and upper node
                float ival = 0.;                                 //  TODO: initialize
                if(!unode) {
                    unode = this->taxinter_.getRoot();
                    uscore = -1;
                    ival = 1.;
                } else if(unode != lnode && lscore < qscore) ival = (qscore - lscore)/static_cast<float>(uscore - lscore);
                
                logsink << std::endl << "    SCORE\tlscore = " << lscore << "; uscore = " << uscore << "; qscore = " << qscore << "; qscore_ex = " << qscore_ex << "; ival = " << ival  << std::endl << std::endl;
                const float taxsig = .0;  // TODO: placer.getTaxSignal(qscore);

                ival_global = std::max(ival, ival_global);  // combine interpolation values conservatively
                anchors_taxsig = std::min(taxsig, anchors_taxsig);  // combine taxonomic signal values conservatively
                unode_global = this->taxinter_.getLCA(unode_global, unode);
                lnode_global = this->taxinter_.getLCA(lnode_global, lnode);
//                 anchors_lnode.push_back(lnode);
//                 anchors_unode.push_back(unode);
                
            } while (! qgroup.empty() && lnode_global != this->taxinter_.getRoot());

            logsink << "    NUMALN\t" << alignments_counter << tab << alignments_counter_naive - alignments_counter << std::endl;
            logsink << "    NUMOUTGRP\t" << outgroup.size() << std::endl;
        
//             { // retain only taxa in outgroup which are closest to root
//                 std::size_t tmp_size = outgroup.size();
//                 for (std::map<uint, small_unsigned_int>::iterator it = outgroup.begin(); it !=  outgroup.end();) {
//                     if(it->second > lca_root_dist_min) outgroup.erase(it++);  // TODO: check if this works
//                     else ++it;
//                 }
//                 logsink << "    NUMOUTGRP\t" << outgroup.size() << tab << tmp_size - outgroup.size() << std::endl;
//             }
            
//         anchors_unode.push_back(unode);
            measure_pass_1_alignment_.stop();
        }

        // avoid unneccessary computation if root node is reached TODO: where?
//         unode = this->taxinter_.getLCA(anchors_unode);
//         anchors_unode.clear();  // TODO: remove; code simplification
        logsink << "    RANGE\t" << rtax->data->annotation->name << tab << lnode_global->data->annotation->name << tab << unode_global->data->annotation->name << std::endl << std::endl;
        
        {   // pass 2 (stable upper node estimation alignment)
            measure_pass_2_alignment_.start();
            logsink << "  PASS\t2" << std::endl;
            uint alignments_counter = 0;
            uint alignments_counter_naive = 0;
            while (! outgroup.empty()) {
//                 std::list<uint> reevaluate;
//                 BandFactor bandfactor2(this->taxinter_, n);
                const uint index_anchor = *outgroup.begin();
                outgroup.erase(outgroup.begin());
                
//                 std::cerr << "after erase: " << outgroup.size() << std::endl;
//                 unode = records_ordered[index_anchor]->getReferenceNode();
//                 lnode = NULL;
//                 bandfactor2.addSequence(0, rnode);
                
                if( unode_global == lca_allnodes ) {
                    if( ! rrseqs_matches[index_anchor] ) alignments_counter_naive += n;
                    else alignments_counter_naive += n - 1;
                    continue;
                }

                if (!rrseqs_matches[index_anchor]) { //need to align query <=> anchor
                    int score = -seqan::globalAlignmentScore(rrseqs_ordered[index_anchor], qrseq, seqan::MyersBitVector());
                    int matches = std::max(seqan::length(rrseqs_ordered[index_anchor]), seqan::length(qrseq)) - score;
                    logsink << "    +ALN query <=> " << index_anchor << "\tscore = " << score << "; matches = " << matches << std::endl;
                    rrseqs_qscores[index_anchor] = score;
                    ++pass_2_counter;
                    ++alignments_counter;
                    ++alignments_counter_naive;
                    rrseqs_matches[index_anchor] = matches;
                }

                const int qscore = rrseqs_qscores[index_anchor];
                const int qscore_ex = qscore*bandfactor_max;
                
//                const TaxonNode* rnode = records_ordered[index_anchor]->getReferenceNode();
                logsink << "      query: (" << qscore_ex << ") unknown" << std::endl;

                // align all others <=> anchor TODO: heuristic
                

                for (uint i = 0; i < n; ++i) {
                    const TaxonNode* cnode = records_ordered[i]->getReferenceNode();
                    int score;
                    if (i == index_anchor) score = 0;
                    else {
                        if( this->taxinter_.isParentOf(unode_global, cnode) || cnode == unode_global ) score = std::numeric_limits<int>::max();
                        else {
                            score = -seqan::globalAlignmentScore(rrseqs_ordered[ i ], rrseqs_ordered[ index_anchor ], seqan::MyersBitVector());
                            logsink << "    +ALN " << i << " <=> " << index_anchor << "\tscore = " << score <<  std::endl;
                            ++pass_2_counter;
                            rrseqs_qscores[i] = score;
                            ++alignments_counter;
                        }
                        ++alignments_counter_naive;
                    }

                    if (score == 0) outgroup.erase(i);
                    
//                     bandfactor2.addSequence(score, cnode);

                    if(score <= qscore_ex) {
                        const TaxonNode* rnode = records_ordered[index_anchor]->getReferenceNode();
                        unode_global = this->taxinter_.getLCA(unode_global, cnode);
                        logsink << "      current upper node: " << "("<< score <<") "<< unode_global->data->annotation->name << " (+ " << cnode->data->annotation->name << " at " << static_cast<int>(this->taxinter_.getLCA(cnode, rnode)->data->root_pathlength) << " )" << std::endl;
                    }
//                     else reevaluate.push_back(i);
                }

//                 {
//                     int qscore_ex = qscore * bandfactor2.getFactor();
//                     logsink << std::endl << "    EXT\tqscore = " << qscore << "; threshold = " << qscore_ex << std::endl;
//                     std::list<uint>::iterator it = reevaluate.begin();
//                     while(it != reevaluate.end() && unode != lca_allnodes ) {
//                         const int score = rrseqs_qscores[*it];
//                         const TaxonNode* cnode = records_ordered[*it]->getReferenceNode();
//                         if(score <= qscore_ex) {
//                             unode = this->taxinter_.getLCA(cnode, unode);
//                             logsink << "      current upper node: " << "("<< score <<") "<< unode->data->annotation->name << " (+ " << cnode->data->annotation->name << " at " << static_cast<int>(this->taxinter_.getLCA(cnode, rnode)->data->root_pathlength) << " )" << std::endl;
//                         }
//                         ++it;
//                     }
//                 }

                logsink << std::endl;
            }
            measure_pass_2_alignment_.stop();
            logsink << "    NUMALN\t" << alignments_counter << tab << alignments_counter_naive - alignments_counter << std::endl;
        }

//         lnode = this->taxinter_.getLCA(anchors_lnode);
//         if(! anchors_unode.empty()) unode = this->taxinter_.getRoot();  // root if no outgroup element
//         else unode = this->taxinter_.getLCA(anchors_unode);
        
        if(unode_global == lnode_global) ival_global = 1.;
        
        logsink << "    RANGE\t" << rtax->data->annotation->name << tab << lnode_global->data->annotation->name << tab << unode_global->data->annotation->name << std::endl << std::endl;

        prec.setSignalStrength(anchors_taxsig);
        prec.setQueryFeatureBegin(qrstart);
        prec.setQueryFeatureEnd(qrstop);
        prec.setInterpolationValue(ival_global);
        prec.setNodeRange(lnode_global, unode_global, anchors_support);
        prec.setBestReferenceTaxon(rtax);
        gcounter = pass_0_counter + pass_1_counter + pass_2_counter;
        float normalised_rt = (float)gcounter/(float)n;
        logsink << "STATS \"" << qrseqname << "\"\t" << n << "\t" << pass_0_counter << "\t"<< pass_1_counter << "\t" << pass_2_counter << "\t" << gcounter << "\t" << std::setprecision(2) << std::fixed <<normalised_rt << std::endl << std::endl;
    }

protected:
    QStorType& query_sequences_;
    const DBStorType& db_sequences_;
    SortByBitscoreFilter< ContainerT > sort_;
    compareTupleFirstLT< boost::tuple< int, uint >, 0 > tuple_1_cmp_le_;

private:
    const float exclude_alignments_factor_;
    const float reeval_bandwidth_factor_;
    StopWatchCPUTime measure_sequence_retrieval_;
    StopWatchCPUTime measure_pass_0_alignment_;
    StopWatchCPUTime measure_pass_1_alignment_;
    StopWatchCPUTime measure_pass_2_alignment_;
};

#endif // taxonpredictionmodelsequence_hh_
