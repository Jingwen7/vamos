#include <iostream>
#include <stdlib.h>
#include <istream>
#include <fstream>
#include <sstream>
#include <assert.h>
#include <string>
#include <tuple> 
#include <vector>
#include "io.h"
#include "read.h"
#include "vntr.h"
#include "vcf.h"
#include "htslib/hts.h"
#include "htslib/sam.h"

using namespace std;

int IO::readMotifsFromCsv (vector<VNTR *> &vntrs) 
{
    int vntr_size = vntrs.size();

    ifstream ifs(motif_csv);
    if (ifs.fail()) 
    {
        cerr << "Unable to open file " << motif_csv << endl;
        return 1;
    }    

    string line;
    int numOfLine = 0;
    while (getline(ifs, line)) 
    {
        stringstream ss(line);
        string tmp;
        while(getline(ss, tmp, ',')) 
        {
            vntrs[numOfLine]->motifs.push_back(MOTIF(tmp));
            // cerr << vntrs[numOfLine]->motifs.back().seq << endl;
        }
        numOfLine += 1; // 0-indexed
    }
    assert(vntrs.size() == numOfLine);
    return 0;
}

int IO::read_tsv(vector<vector<string>> &items) 
{
    items.clear();
    ifstream ifs(vntr_bed);
    if (ifs.fail()) 
    {
        cerr << "Unable to open file " << vntr_bed << endl;
        return 1;
    }

    string line;
    while (getline(ifs, line)) 
    {
        stringstream ss(line);
        vector<string> item;
        string tmp;
        while(getline(ss, tmp, '\t')) 
            item.push_back(tmp);

        // for (auto &i : item)
        //     cerr << i << "\t";
        // cerr << endl;
        items.push_back(item);
    }
    return 0;
}

/* read vntrs coordinates from file `vntr_bed`*/
void IO::readVNTRFromBed (vector<VNTR*> &vntrs)
{
    vector<vector<string>> items;
    read_tsv(items);
    uint32_t start, end, len;
    for (auto &it : items)
    {
        start = stoi(it[1]);
        end = stoi(it[2]);
        len = start < end ? end - start : 0;
        VNTR * vntr = new VNTR(it[0], start, end, len);
        vntrs.push_back(vntr);
    }
    return;
}

pair<uint32_t, bool> processCigar(bam1_t * aln, uint32_t * cigar, uint32_t CIGAR_start, uint32_t target_crd, uint32_t &ref_aln_start, uint32_t &read_aln_start)
{
    assert(read_aln_start <= aln->core.l_qseq);
    /* trivial case: when ref_aln_start equals target_crd*/
    if (target_crd == ref_aln_start) return make_pair(read_aln_start, 1);

    uint32_t cigar_start = CIGAR_start;
    uint32_t k; int op, l;
    for (k = cigar_start; k < aln->core.n_cigar; ++k) 
    {
        op = bam_cigar_op(cigar[k]);
        l = bam_cigar_oplen(cigar[k]);

        if (read_aln_start > aln->core.l_qseq)
            return make_pair(read_aln_start, 0);
        if (ref_aln_start > target_crd) 
            return make_pair(read_aln_start, 0); // skip the out-of-range alignment
        else if (ref_aln_start == target_crd)
            return make_pair(read_aln_start, 1);
        else if (target_crd < ref_aln_start + l)
        {
            if (op == BAM_CMATCH) 
                return make_pair(read_aln_start + target_crd - ref_aln_start, 1);
            else
                return make_pair(read_aln_start, 1);
            break;
        }

        CIGAR_start = k;
        switch (op) 
        {
            case BAM_CDEL:
                ref_aln_start += l;
                break;    

            case BAM_CINS:
                read_aln_start += l;  
                break;            

            case BAM_CREF_SKIP:
                ref_aln_start += l;
                break; 

            case BAM_CMATCH:
                read_aln_start += l;
                ref_aln_start += l;
                break;

            case BAM_CEQUAL:
                read_aln_start += l;
                ref_aln_start += l;
                break;

            case BAM_CDIFF:
                read_aln_start += l;
                ref_aln_start += l;
                break;

            case BAM_CHARD_CLIP:
                break;

            case BAM_CPAD:
                break;

            case BAM_CSOFT_CLIP:
                read_aln_start += l;
                break;
        }
        // cerr << "read_aln_start: " << read_aln_start << " op: " << op << endl;
    }
    assert(read_aln_start <= aln->core.l_qseq);
    return make_pair(read_aln_start, 1);
}

/*
 read alignment from bam
 liftover ref_VNTR_start, ref_VNTR_end of every vntr
 get the subsequence 
*/
void IO::readSeqFromBam (vector<VNTR *> &vntrs) 
{
    char * bai = (char *) malloc(strlen(input_bam) + 4 + 1); // input_bam.bai
    strcpy(bai, input_bam);
    strcat(bai, ".bai");

    samFile * fp_in = hts_open(input_bam, "r"); //open bam file
    bam_hdr_t * bamHdr = sam_hdr_read(fp_in); //read header
    hts_idx_t * idx = sam_index_load(fp_in, bai);
    bam1_t * aln = bam_init1(); //initialize an alignment
    hts_itr_t * itr;

    uint32_t isize; // observed template size
    uint32_t * cigar;
    uint8_t * s; // pointer to the read sequence
    bool rev;

    uint32_t ref_aln_start, ref_aln_end;
    uint32_t read_aln_start, read_len;
    uint32_t ref_len;
    uint32_t VNTR_s, VNTR_e;
    uint32_t k;
    uint32_t liftover_read_s, liftover_read_e;

    for (auto &vntr : vntrs)
    {
        cerr << "starting processing vntr" << endl;
        itr = bam_itr_querys(idx, bamHdr, vntr->region.c_str());
        while(bam_itr_next(fp_in, itr, aln) >= 0)
        {
            rev = bam_is_rev(aln);
            if (aln->core.flag & BAM_FSECONDARY or aln->core.flag & BAM_FUNMAP) 
            {
                cerr << "       skip secondary and unmmaped" << endl;
                continue; // skip secondary alignment / unmapped reads
            }

            if (aln->core.qual < 20) 
            {
                cerr << "       skip low mapq" << endl;
                continue; // skip alignment with mapping qual < 20
            }

            isize = aln->core.isize;
            if (isize == 0) // 9th col: Tlen is unavailable in bam
                isize = bam_endpos(aln);

            if (isize > 0 and isize < vntr->len) {
                cerr << "       skip short size" << endl;
                continue; // skip alignment with length < vntr->len
            }

            // cerr << "starting lifting read " << bam_get_qname(aln) << endl;

            cigar = bam_get_cigar(aln);
            ref_aln_start = aln->core.pos;
            ref_aln_end = ref_aln_start + isize;
            ref_len = bamHdr->target_len[aln->core.tid]; 

            read_aln_start = 0;
            read_len = aln->core.l_qseq;

            uint32_t tmp;
            VNTR_s = vntr->ref_start;
            VNTR_e = vntr->ref_end;

            if (rev) 
            {   
                tmp = VNTR_s;
                VNTR_s = ref_len - VNTR_e;
                VNTR_e = ref_len - tmp - 1;

                tmp = ref_aln_start;
                ref_aln_start = ref_len - ref_aln_end;
                ref_aln_end = ref_len - tmp - 1;
            }

            if (VNTR_s < ref_aln_start or VNTR_e > ref_aln_end) // the alignment doesn't fully cover the VNTR locus
                continue;

            /*
            reference: VNTR_s, VNTR_e
            read alignment: ref_aln_start, ref_aln_end; read_aln_start, read_aln_end;
            */
            uint32_t cigar_start = 0;
            auto [liftover_read_s, iflift_s] = processCigar(aln, cigar, cigar_start, VNTR_s, ref_aln_start, read_aln_start);
            auto [liftover_read_e, iflift_e] = processCigar(aln, cigar, cigar_start, VNTR_e, ref_aln_start, read_aln_start);

            // [liftover_read_s, liftover_read_e]
            if (iflift_s and iflift_e and liftover_read_e > liftover_read_s and liftover_read_e <= read_len)
            {
                liftover_read_s = liftover_read_s == 0 ? 0 : liftover_read_s - 1;
                liftover_read_e = liftover_read_e == 0 ? 0 : liftover_read_e - 1;

                assert(liftover_read_e < read_len);

                READ * read = new READ();
                read->chr = bamHdr->target_name[aln->core.tid]; 
                read->qname = bam_get_qname(aln);
                read->len = liftover_read_e - liftover_read_s + 1; // read length
                read->seq = (char *) malloc(read->len + 1); // read sequence array
                s = bam_get_seq(aln); 

                for(uint32_t i = 0; i < read->len; i++)
                {
                    assert(i + liftover_read_s < read_len);
                    assert(bam_seqi(s, i + liftover_read_s) < 16);
                    read->seq[i] = seq_nt16_str[bam_seqi(s, i + liftover_read_s)]; //gets nucleotide id and converts them into IUPAC id.
                }
                vntr->reads.push_back(read); 
                // cerr << "read_name: " << bam_get_qname(aln) << endl; 
                // cerr << "vntr->ref_start: " << vntr->ref_start << " vntr->ref_end: " << vntr->ref_end << endl;
                // cerr << "liftover_read_s: " << liftover_read_s << " liftover_read_e: " << liftover_read_e << endl;
                // cerr << "read length: " << aln->core.l_qseq << endl;
                // cerr << "read_seq: " << read->seq << endl; 
            }
        }
        vntr->nreads = vntr->reads.size();
        cerr << "vntr read size: " << vntr->nreads << endl;
    }
    free(bai);
    bam_destroy1(aln);
    bam_hdr_destroy(bamHdr);
    hts_itr_destroy(itr);
    sam_close(fp_in); 
    return;  
}

void VcfWriteHeader(ostream& out, VcfWriter & vcfWriter)
{
    vcfWriter.writeHeader(out);
    return;
}

void VCFWriteBody(vector<VNTR *> &vntrs, VcfWriter & vcfWriter, ostream& out)
{
    vcfWriter.writeBody(vntrs, out);
    return;
}

int IO::outputVCF (vector<VNTR *> &vntrs)
{
    VcfWriter vcfWriter(input_bam, version, sampleName);

    ofstream out(out_vcf);
    if (out.fail()) 
    {
        cerr << "Unable to open file " << out_vcf << endl;
        return 1;
    }
    VcfWriteHeader(out, vcfWriter);
    VCFWriteBody(vntrs, vcfWriter, out);
    out.close();  
    return 0;
}
