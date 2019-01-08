/*
============================================================================
Tracy: Trace File Handling
============================================================================
Copyright (C) 2017,2018 Tobias Rausch

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
============================================================================
Contact: Tobias Rausch (rausch@embl.de)
============================================================================
*/

#ifndef INDIGO_H
#define INDIGO_H

#define BOOST_DISABLE_ASSERTS
#include <boost/multi_array.hpp>
#include "decompose.h"
#include "trim.h"
#include "web.h"
#include "variants.h"

using namespace sdsl;

namespace tracy {
  
  struct IndigoConfig {
    bool callvariants;
    uint16_t linelimit;
    uint16_t trimLeft;
    uint16_t trimRight;
    uint16_t kmer;
    uint16_t maxindel;
    uint16_t madc;
    uint16_t minKmerSupport;
    float pratio;
    std::string format;
    boost::filesystem::path outfile;
    boost::filesystem::path ab;
    boost::filesystem::path genome;
  };

  int indigo(int argc, char** argv) {
    IndigoConfig c;
  
    // Parameter
    boost::program_options::options_description generic("Generic options");
    generic.add_options()
      ("help,?", "show help message")
      ("genome,g", boost::program_options::value<boost::filesystem::path>(&c.genome), "(gzipped) fasta or wildtype ab1 file")
      ("pratio,p", boost::program_options::value<float>(&c.pratio)->default_value(0.33), "peak ratio to call base")
      ("kmer,k", boost::program_options::value<uint16_t>(&c.kmer)->default_value(15), "kmer size")
      ("support,s",  boost::program_options::value<uint16_t>(&c.minKmerSupport)->default_value(3), "min. kmer support")
      ("maxindel,m", boost::program_options::value<uint16_t>(&c.maxindel)->default_value(1000), "max. indel size in Sanger trace")
      ("trimLeft,l", boost::program_options::value<uint16_t>(&c.trimLeft)->default_value(50), "trim size left")
      ("trimRight,r", boost::program_options::value<uint16_t>(&c.trimRight)->default_value(50), "trim size right")
      ("callVariants,v", "call variants in trace")
      ;
    
    boost::program_options::options_description otp("Output options");
    otp.add_options()
      ("linelimit,n", boost::program_options::value<uint16_t>(&c.linelimit)->default_value(60), "alignment line length")
      ("format,f", boost::program_options::value<std::string>(&c.format)->default_value("json"), "output format [json|align]")
      ("outfile,o", boost::program_options::value<boost::filesystem::path>(&c.outfile)->default_value("out.json"), "output file")
      ;
    
    boost::program_options::options_description hidden("Hidden options");
    hidden.add_options()
      ("madc,c", boost::program_options::value<uint16_t>(&c.madc)->default_value(5), "MAD cutoff")
      ("input-file", boost::program_options::value<boost::filesystem::path>(&c.ab), "ab1")
      ;
    
    boost::program_options::positional_options_description pos_args;
    pos_args.add("input-file", -1);
    
    boost::program_options::options_description cmdline_options;
    cmdline_options.add(generic).add(otp).add(hidden);
    boost::program_options::options_description visible_options;
    visible_options.add(generic).add(otp);
    boost::program_options::variables_map vm;
    boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(cmdline_options).positional(pos_args).run(), vm);
    boost::program_options::notify(vm);
    
    // Check command line arguments
    if ((vm.count("help")) || (!vm.count("input-file"))) {
      std::cout << "Usage: tracy " << argv[0] << " [OPTIONS] trace.ab1" << std::endl;
      std::cout << visible_options << "\n";
      return -1;
    }
    if (c.maxindel < 1) c.maxindel = 1;

    // Variant calling
    if (vm.count("callVariants")) c.callvariants = true;
    else c.callvariants = false;
    
    // Check ab1
    if (!(boost::filesystem::exists(c.ab) && boost::filesystem::is_regular_file(c.ab) && boost::filesystem::file_size(c.ab))) {
      std::cerr << "Trace file is missing: " << c.ab.string() << std::endl;
      return 1;
    }
    
    // Check reference
    if (!(boost::filesystem::exists(c.genome) && boost::filesystem::is_regular_file(c.genome) && boost::filesystem::file_size(c.genome))) {
      std::cerr << "Reference file is missing: " << c.genome.string() << std::endl;
      return 1;
    }
    
    // Show cmd
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] ";
    std::cout << "tracy ";
    for(int i=0; i<argc; ++i) { std::cout << argv[i] << ' '; }
    std::cout << std::endl;
    
    // Load *.ab1 file
    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Load ab1 file" << std::endl;
    Trace tr;
    int32_t ft = traceFormat(c.ab.string());
    if (ft == 0) {
      if (!readab(c.ab.string(), tr)) return -1;
    } else if (ft == 1) {
      if (!readscf(c.ab.string(), tr)) return -1;
    } else {
      std::cerr << "Unknown trace file type!" << std::endl;
      return -1;
    }

    // Call bases
    BaseCalls bc;
    basecall(tr, bc, c.pratio);
    if (c.format == "align") traceTxtOut(c.outfile.string() + ".abif", bc, tr, c.trimLeft, c.trimRight);

    // Create trimmed trace profile
    typedef boost::multi_array<float, 2> TProfile;
    TProfile ptrace;
    createProfile(tr, bc, ptrace, c.trimLeft, c.trimRight);

    // Identify position of indel shift in Sanger trace
    TraceBreakpoint bp;
    findBreakpoint(ptrace, bp);
    
    // Load reference
    csa_wt<> fm_index;
    ReferenceSlice rs;
    if (!loadFMIdx(c, rs, fm_index)) return -1;
      
    // Find reference match
    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Find Reference Match" << std::endl;
    
    // Get reference slice
    if (!getReferenceSlice(c, fm_index, bc, rs)) return -1;

    // Create reference profile
    TProfile prefslice;
    createProfile(c, rs, prefslice);

    // Semi-global alignment
    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Alignment" << std::endl;
    typedef boost::multi_array<char, 2> TAlign;
    TAlign align;
    AlignConfig<true, false> semiglobal;
    DnaScore<int> sc(5, -4, -10, -1);
    gotoh(ptrace, prefslice, align, semiglobal, sc);
    
    // Do we have a shifted trace?
    if (!bp.indelshift) {
      // Find breakpoint for hom. indels
      now = boost::posix_time::second_clock::local_time();
      std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Homozygous InDel Search" << std::endl;
      if (!findHomozygousBreakpoint(align, bp)) return -1;
    }

    // Debug Breakpoint & Alignment
    //std::cerr << "Breakpoint: " << bp.indelshift << ',' << bp.traceleft << ',' << bp.breakpoint << ',' << bp.bestDiff << std::endl;
    //for(uint32_t i = 0; i<align.shape()[0]; ++i) {
    //uint32_t alignedNuc = 0;
    //for(uint32_t j = 0; j<align.shape()[1]; ++j) {
    //if (align[0][j] != '-') {
    //++alignedNuc;
    //if (alignedNuc == bp.breakpoint) std::cerr << "#####";
    //}
    //std::cerr << align[i][j];
    //}
    //std::cerr << std::endl;
    //}    

    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Decompose Chromatogram" << std::endl;
    
    // Decompose alleles
    typedef std::pair<int32_t, int32_t> TIndelError;
    typedef std::vector<TIndelError> TDecomposition;
    TDecomposition dcp;
    if (!decomposeAlleles(c, align, bc, bp, rs, dcp)) return -1;
    if (c.format == "align") writeDecomposition(c, dcp);

    // Generate plain nucleotide sequence for second allele
    generateSecondaryDecomposed(tr, bc);

    // Estimate allelic fractions
    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Estimate allelic fractions" << std::endl;
    typedef std::pair<double, double> TFractions;
    TFractions a1a2 = allelicFraction(c, tr, bc);
    
    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Align to reference" << std::endl;
    
    // Allele1
    typedef boost::multi_array<char, 2> TAlign;
    TAlign alignPrimary;
    std::string pri = trimmedSeq(bc.primary, c.trimLeft, c.trimRight);
    gotoh(pri, rs.refslice, alignPrimary, semiglobal, sc);
    // Trim initial reference slice
    ReferenceSlice allele1(rs);
    trimReferenceSlice(c, alignPrimary, allele1);
    typedef boost::multi_array<char, 2> TAlign;
    TAlign final1;
    int32_t a1Score = gotoh(pri, allele1.refslice, final1, semiglobal, sc);
    if (c.format == "align") plotAlignment(c, final1, allele1, 1, a1Score, a1a2);

    // Allele2
    TAlign alignSecondary;
    std::string sec = trimmedSeq(bc.secDecompose, c.trimLeft, c.trimRight);
    gotoh(sec, rs.refslice, alignSecondary, semiglobal, sc);
    // Trim initial reference slice
    ReferenceSlice allele2(rs);
    trimReferenceSlice(c, alignSecondary, allele2);
    TAlign final2;
    int32_t a2Score = gotoh(sec, allele2.refslice, final2, semiglobal, sc);
    if (c.format == "align") plotAlignment(c, final2, allele2, 2, a2Score, a1a2);

    // Allele1 vs. Allele2
    TAlign final3;
    AlignConfig<false, false> global;
    ReferenceSlice secrs;
    secrs.refslice = sec;
    secrs.forward = 1;
    secrs.pos = 0;
    secrs.chr = "Alt2";
    int32_t a3Score = gotoh(pri, secrs.refslice, final3, global, sc);
    if (c.format == "align") plotAlignment(c, final3, secrs, 3, a3Score, a1a2);

    // Any het. InDel
    if (!bp.indelshift) {
      // Center on first SNP
      uint32_t reliableTracePos = findBestTraceSection(bc);
      bp.breakpoint = nearestSNP(c, bc, reliableTracePos);
    }

    // Variant Calling
    if (c.callvariants) {
      now = boost::posix_time::second_clock::local_time();
      std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Variant Calling" << std::endl;
      
      typedef std::vector<Variant> TVariants;
      TVariants var;
      callVariants(c, final1, allele1, var);
      callVariants(c, final2, allele2, var);

      now = boost::posix_time::second_clock::local_time();
      std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Variant Annotation" << std::endl;
      //std::string response;
      //if (!variantsInRegion("17:80348215-80348333", response)) {
      //parseJSON(response);
      //}

      // Sort variants
      std::sort(var.begin(), var.end(), SortVariant<Variant>());

      // VCF output
      vcfOutput(c, var, rs);
    }
    
    // Json output
    traceAlleleAlignJsonOut(c, bc, tr, allele1, allele2, secrs, final1, final2, final3, dcp, a1Score, a2Score, a3Score, bp, a1a2);

    now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Done." << std::endl;
    return 0;
  }

}

#endif
