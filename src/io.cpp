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
            cerr << vntrs[numOfLine]->motifs.back().seq << endl;
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
        {
            item.push_back(tmp);
        }

        for (auto &i : item)
        {
            cerr << i << "\t";
        }
        cerr << endl;
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

pair<uint32_t, bool> processCigar(bam1_t * aln, uint32_t * cigar, uint32_t &cigar_start, uint32_t target_crd, uint32_t &ref_aln_start, uint32_t &read_aln_start)
{
    /* trivial case: when ref_aln_start equals target_crd*/
    if (target_crd == ref_aln_start) return make_pair(read_aln_start, 1);

    uint32_t k; int op, l;
    for (k = cigar_start; k < aln->core.n_cigar; ++k) 
    {
        op = bam_cigar_op(cigar[k]);
        l = bam_cigar_oplen(cigar[k]);

        if (ref_aln_start > target_crd) {
            cigar_start = k;
            return make_pair(read_aln_start, 0); // skip the out-of-range alignment
        }
        else if (ref_aln_start <= target_crd and target_crd < ref_aln_start + l)
        {
            // if (op == BAM_CMATCH) 
                // read_aln_start += target_crd - ref_aln_start;
            cigar_start = k;
            if (op == BAM_CMATCH) 
                return make_pair(read_aln_start + target_crd - ref_aln_start, 1);
            else
                return make_pair(read_aln_start, 1);
            break;
        }

        switch (op) 
        {
        case BAM_CDEL:
            read_aln_start += l;  
            break;    

         case BAM_CINS:
            ref_aln_start += l;
            break;            

         case BAM_CMATCH:
            read_aln_start += l;
            ref_aln_start += l;
            break;

         case BAM_CDIFF:
            read_aln_start += l;
            ref_aln_start += l;
            break;

         case BAM_CHARD_CLIP:
            break;

         case BAM_CSOFT_CLIP:
            read_aln_start += l;
            break;
        }
    }
    return make_pair(read_aln_start, 1);
}

/*
 read alignment from bam overlapping a VNTR locus
 liftover ref_VNTR_start, ref_VNTR_end
 get the subsequence 
*/
void IO::readSeqFromBam (vector<READ*> &reads, string &chr, const uint32_t &ref_VNTR_start, 
                       const uint32_t &ref_VNTR_end, const uint32_t &VNTR_len, string &region) 
{
    char * bai = (char *) malloc(strlen(input_bam) + 4 + 1); // input_bam.bai
    strcpy(bai, input_bam);
    strcat(bai, ".bai");

    samFile * fp_in = hts_open(input_bam, "r"); //open bam file
    bam_hdr_t * bamHdr = sam_hdr_read(fp_in); //read header
    hts_idx_t * idx = sam_index_load(fp_in, bai);
    bam1_t * aln = bam_init1(); //initialize an alignment
    hts_itr_t * itr = bam_itr_querys(idx, bamHdr, region.c_str());

    uint16_t flag;
    uint32_t mapq;
    uint32_t isize; // observed template size
    uint32_t * cigar;
    uint8_t * s; // pointer to the read sequence

    uint32_t ref_aln_start, ref_aln_end;
    uint32_t read_aln_start, read_aln_end;
    uint32_t ref_len;
    uint32_t VNTR_s, VNTR_e;
    uint32_t k;
    uint32_t liftover_read_s, liftover_read_e;

    while(bam_itr_next(fp_in, itr, aln) >= 0)
    {
        flag = aln->core.flag;
        if (flag & BAM_FSECONDARY or flag & BAM_FUNMAP) continue; // skip secondary alignment / unmapped reads

        mapq = aln->core.qual;
        if (mapq < 20) continue; // skip alignment with mapping qual < 20

        isize = aln->core.isize;
        if (isize < VNTR_len) continue; // skip alignment with length < VNTR_len

        cigar = bam_get_cigar(aln);
        ref_aln_start = aln->core.pos;
        ref_aln_end = ref_aln_start + isize;

        read_aln_start = 0;
        read_aln_end = aln->core.l_qseq;
        ref_len = bamHdr->target_len[aln->core.tid]; 

        uint32_t tmp;
        VNTR_s = ref_VNTR_start;
        VNTR_e = ref_VNTR_end;
        if (bam_is_rev(aln)) 
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

        if (iflift_s and iflift_e and liftover_read_e > liftover_read_s)
        {

            READ * read = new READ();
            read->chr = bamHdr->target_name[aln->core.tid]; 
            read->qname = bam_get_qname(aln);
            read->len = liftover_read_e - liftover_read_s; // read length
            read->seq = (char *) malloc(read->len + 1); // read sequence array
            s = bam_get_seq(aln); 

            assert(liftover_read_e <= aln->core.l_qseq);

            for(uint32_t i = 0; i < liftover_read_e - liftover_read_s; i++)
            {
                assert(i + liftover_read_s < aln->core.l_qseq);
                assert(bam_seqi(s, i + liftover_read_s) < 16);
                read->seq[i] = seq_nt16_str[bam_seqi(s, i + liftover_read_s)]; //gets nucleotide id and converts them into IUPAC id.
            }
            reads.push_back(read); 
            // cerr << "read_name: " << bam_get_qname(aln) << endl; 
            // cerr << "ref_VNTR_start: " << ref_VNTR_start << " ref_VNTR_end: " << ref_VNTR_end << endl;
            // cerr << "liftover_read_s: " << liftover_read_s << " liftover_read_e: " << liftover_read_e << endl;
            // cerr << "read length: " << aln->core.l_qseq << endl;
            // cerr << "read_seq: " << read->seq << endl; 
        }
    }
    free(bai);
    bam_destroy1(aln);
    hts_itr_destroy(itr);
    bam_hdr_destroy(bamHdr);
    sam_close(fp_in);   
}

void IO::readSeq (VNTR * vntr) 
{
    readSeqFromBam(vntr->reads, vntr->chr, vntr->ref_start, vntr->ref_end, vntr->len, vntr->region);
    vntr->nreads = vntr->reads.size();
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
