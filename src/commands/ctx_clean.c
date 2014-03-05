#include "global.h"

#include "commands.h"
#include "util.h"
#include "file_util.h"
#include "db_graph.h"
#include "graph_info.h"
#include "graph_format.h"
#include "clean_graph.h"
#include "supernode.h" // for saving length histogram

const char clean_usage[] =
"usage: "CMD" clean [options] <in.ctx> [in2.ctx ...]\n"
" Clean a cortex graph. Joins graphs first, if multiple inputs given\n"
" Clips tips before doing supernode thresholding (when doing both [default]).\n"
"\n"
" Options:\n"
"  --memory <mem>         Memory to use\n"
"  --nkmers <hash-size>   Kmers in the hash table (e.g. 1G ~ 1 billion)\n"
"  --ncols <colour>       Number of samples in memory at once (speedup)\n"
"  --tips <L>             Clip tips shorter than <L> kmers\n"
"  --supernodes           Remove low coverage supernode. Additional options:\n"
"    --kdepth <C>         kmer depth: (depth*(R-Kmersize+1)/R); R = read length\n"
"    --threshold <T>      Cleaning threshold, remove supnodes where [coverage < T]\n"
" Output:\n"
"  --out <out.ctx>        Save output graph file\n"
"  --covgs <out.csv>      Dump covg distribution before cleaning to a CSV file\n"
"  --len-before <out.csv> Write supernode length before cleaning\n"
"  --len-after <out.csv>  Write supernode length before cleaning\n"
"\n"
" Default: --tips 2*kmer_size --supernodes\n";

// Size of length histogram is 2000 kmers
#define LEN_HIST_CAP 2000

#ifdef CTXVERBOSE
  #define DEBUG_SUPERNODE 1
#endif

int ctx_clean(CmdArgs *args)
{
  int argc = args->argc;
  char **argv = args->argv;
  // Already checked that we have at least 2 arguments

  // Check cmdline args
  bool tip_cleaning = false, supernode_cleaning = false;
  size_t max_tip_len = 0;
  Covg threshold = 0;
  double seq_depth = -1;
  const char *dump_covgs = NULL;
  const char *len_before_path = NULL, *len_after_path = NULL;

  int argi;
  for(argi = 0; argi < argc && argv[argi][0] == '-'; argi++) {
    if(strcmp(argv[argi],"--tips") == 0) {
      if(argi + 1 >= argc || !parse_entire_size(argv[argi+1], &max_tip_len) ||
         max_tip_len <= 1) {
        cmd_print_usage("--tips <L> needs an integer argument > 1");
      }
      tip_cleaning = true;
      argi++;
    }
    else if(strcmp(argv[argi],"--supernodes") == 0) supernode_cleaning = true;
    else if(strcmp(argv[argi],"--covgs") == 0) {
      if(argi + 1 >= argc) cmd_print_usage("--covgs <out.csv> needs an argument");
      dump_covgs = argv[argi+1];
      argi++;
    }
    else if(strcmp(argv[argi],"--threshold") == 0) {
      if(argi+1 >= argc || !parse_entire_uint(argv[argi+1], &threshold) ||
         threshold <= 1) {
        cmd_print_usage("--threshold <T> needs an integer argument > 1");
      }
      argi++;
    }
    else if(strcmp(argv[argi],"--kdepth") == 0) {
      if(argi+1 >= argc || !parse_entire_double(argv[argi+1], &seq_depth) ||
         seq_depth <= 1) {
        cmd_print_usage("--kdepth <C> needs a positive decimal number > 1");
      }
      argi++;
    }
    else if(strcmp(argv[argi],"--len-before") == 0) {
      if(argi+1 >= argc) cmd_print_usage("--len-before <out.csv> needs a path");
      len_before_path = argv[argi+1];
      argi++;
    }
    else if(strcmp(argv[argi],"--len-after") == 0) {
      if(argi+1 >= argc) cmd_print_usage("--len-after <out.csv> needs a path");
      len_after_path = argv[argi+1];
      argi++;
    }
    else cmd_print_usage("Unknown argument: %s", argv[argi]);
  }

  char *out_ctx_path = args->output_file_set ? args->output_file : NULL;

  if(argi == argc) cmd_print_usage("Please give input graph files");

  // Default behaviour
  if(!tip_cleaning && !supernode_cleaning) {
    if(out_ctx_path != NULL)
      supernode_cleaning = tip_cleaning = true; // do both
    else
      warn("No cleaning being done: you did not specify --out <out.ctx>");
  }

  bool doing_cleaning = (supernode_cleaning || tip_cleaning);

  if(doing_cleaning && out_ctx_path == NULL) {
    cmd_print_usage("Please specify --out <out.ctx> for cleaned graph");
  }

  if(!supernode_cleaning && threshold > 0)
    cmd_print_usage("--threshold <T> not needed if not cleaning with --supernodes");
  if(!supernode_cleaning && seq_depth > 0)
    cmd_print_usage("--kdepth <C> not needed if not cleaning with --supernodes");

  if(supernode_cleaning && threshold != 0 && seq_depth > 0) {
    cmd_print_usage("supernode cleaning requires only one of "
                             "--threshold <T>, --depth <D>");
  }

  if(!doing_cleaning && len_after_path) {
    cmd_print_usage("You use --len-after <out.csv> without any cleaning"
                    " (set --supernodes or --tips)");
  }

  if(out_ctx_path != NULL && strcmp(out_ctx_path,"-") != 0 &&
     futil_file_exists(out_ctx_path))
  {
    cmd_print_usage("Output file already exists: %s", out_ctx_path);
  }

  // Use remaining args as graph files
  char **paths = argv + argi;
  size_t i, j, num_files = (size_t)(argc - argi), total_cols = 0;

  // Open graph files
  GraphFileReader files[num_files];
  uint64_t max_ctx_kmers = 0;

  for(i = 0; i < num_files; i++)
  {
    files[i] = INIT_GRAPH_READER;
    graph_file_open(&files[i], paths[i], true);

    if(files[0].hdr.kmer_size != files[i].hdr.kmer_size) {
      cmd_print_usage("Kmer sizes don't match [%u vs %u]",
                  files[0].hdr.kmer_size, files[i].hdr.kmer_size);
    }

    size_t offset = total_cols;
    total_cols += graph_file_usedcols(&files[i]);
    file_filter_update_intocol(&files[i].fltr, files[i].fltr.intocol + offset);

    max_ctx_kmers = MAX2(max_ctx_kmers, files[i].num_of_kmers);
  }

  size_t use_ncols = args->use_ncols, kmer_size = files[0].hdr.kmer_size;

  // Flatten if we don't have to remember colours / output a graph
  if(!doing_cleaning)
  {
    total_cols = use_ncols = 1;
    for(i = 0; i < num_files; i++)
      file_filter_update_intocol(&files[i].fltr, 0);
  }

  if(total_cols < use_ncols) {
    warn("I only need %zu colour%s ('--ncols %zu' ignored)",
         total_cols, (total_cols != 1 ? "s" : ""), use_ncols);
    use_ncols = total_cols;
  }

  // If no arguments given we default to removing tips < 2*kmer_size
  if(tip_cleaning && max_tip_len == 0)
    max_tip_len = 2 * kmer_size;

  // Warn if any files already cleaned
  size_t fromcol, intocol;
  ErrorCleaning *cleaning;

  for(i = 0; i < num_files; i++) {
    for(j = 0; j < files[i].fltr.ncols; j++) {
      fromcol = graph_file_fromcol(&files[i], j);
      cleaning = &files[i].hdr.ginfo[fromcol].cleaning;
      if(cleaning->cleaned_snodes && supernode_cleaning) {
        warn("%s:%zu already has supernode cleaning with threshold: <%zu",
             files[i].fltr.file_path.buff, fromcol,
             (size_t)cleaning->clean_snodes_thresh);
      }
      if(cleaning->cleaned_tips && tip_cleaning) {
        warn("%s:%zu already has had tip cleaned",
             files[i].fltr.file_path.buff, fromcol);
      }
    }
  }

  // Print steps
  size_t step = 0;
  status("Actions:\n");
  if(len_before_path != NULL)
    status("%zu. Saving supernode length distribution to: %s", step++, len_before_path);
  if(tip_cleaning)
    status("%zu. Cleaning tips shorter than %zu nodes", step++, max_tip_len);
  if(dump_covgs != NULL)
    status("%zu. Saving coverage distribution to: %s", step++, dump_covgs);
  if(supernode_cleaning && threshold > 0)
    status("%zu. Cleaning supernodes with coverage < %u", step++, threshold);
  if(supernode_cleaning && threshold == 0)
    status("%zu. Cleaning supernodes with auto-detected threshold", step++);
  if(len_after_path != NULL)
    status("%zu. Saving supernode length distribution to: %s", step++, len_after_path);

  //
  // Decide memory usage
  //
  bool all_colours_loaded = (total_cols <= use_ncols);

  size_t kmers_in_hash, extra_bits_per_kmer, graph_mem;
  extra_bits_per_kmer = (sizeof(Covg) + sizeof(Edges)) * 8 * use_ncols +
                        (!all_colours_loaded) * sizeof(Edges) * 8;
  kmers_in_hash = cmd_get_kmers_in_hash(args, extra_bits_per_kmer,
                                        max_ctx_kmers, true, &graph_mem);

  cmd_check_mem_limit(args, graph_mem);

  //
  // Check output files are writable
  //
  if(out_ctx_path != NULL && strcmp(out_ctx_path,"-") != 0 &&
     !futil_is_file_writable(out_ctx_path)) {
    cmd_print_usage("Cannot write to output: %s", out_ctx_path);
  }

  if(dump_covgs && !futil_is_file_writable(dump_covgs))
    cmd_print_usage("Cannot write coverage distribution to: %s", dump_covgs);

  FILE *len_before_fh = NULL, *len_after_fh = NULL;

  if(len_before_path && (len_before_fh = fopen(len_before_path, "w")) == NULL)
    die("Cannot write to file 'before' length histogram: %s", len_before_path);
  if(len_after_path && (len_after_fh = fopen(len_after_path, "w")) == NULL)
    die("Cannot write to file 'after' length histogram: %s", len_after_path);

  // Create db_graph
  // Load as many colours as possible
  // Use an extra set of edge to take intersections
  dBGraph db_graph;
  db_graph_alloc(&db_graph, files[0].hdr.kmer_size, use_ncols, use_ncols, kmers_in_hash);
  Edges *edge_store = calloc2(db_graph.ht.capacity * (use_ncols+!all_colours_loaded),
                              sizeof(Edges));
  db_graph.col_edges = edge_store;
  db_graph.col_covgs = calloc2(db_graph.ht.capacity * use_ncols, sizeof(Covg));

  // Load graph into a single colour
  LoadingStats stats;
  loading_stats_init(&stats);

  GraphLoadingPrefs gprefs = {.db_graph = &db_graph,
                              .boolean_covgs = false,
                              .must_exist_in_graph = false,
                              .must_exist_in_edges = NULL,
                              .empty_colours = false};

  // Construct cleaned graph header
  GraphFileHeader outhdr = {.version = CTX_GRAPH_FILEFORMAT,
                            .kmer_size = (uint32_t)db_graph.kmer_size,
                            .num_of_bitfields = NUM_BKMER_WORDS,
                            .num_of_cols = (uint32_t)total_cols,
                            .capacity = 0};

  graph_header_alloc(&outhdr, total_cols);

  // Merge info into header
  size_t gcol = 0;
  for(i = 0; i < num_files; i++) {
    for(j = 0; j < files[i].fltr.ncols; j++, gcol++) {
      fromcol = graph_file_fromcol(&files[i], j);
      intocol = graph_file_intocol(&files[i], j);
      graph_info_merge(&outhdr.ginfo[intocol], &files[i].hdr.ginfo[fromcol]);
    }
  }

  if(total_cols > use_ncols)
  {
    // Load into one colour
    size_t tmpinto; bool tmpflatten;
    for(i = 0; i < num_files; i++)
    {
      tmpinto = files[i].fltr.intocol; tmpflatten = files[i].fltr.flatten;
      file_filter_update_intocol(&files[i].fltr, 0);
      files[i].fltr.flatten = true;
      graph_load(&files[i], gprefs, &stats);
      file_filter_update_intocol(&files[i].fltr, tmpinto);
      files[i].fltr.flatten = tmpflatten;
    }
  }
  else {
    for(i = 0; i < num_files; i++)
      graph_load(&files[i], gprefs, &stats);
  }

  char num_kmers_str[100];
  ulong_to_str(db_graph.ht.num_kmers, num_kmers_str);
  status("Total kmers loaded: %s\n", num_kmers_str);

  size_t initial_nkmers = db_graph.ht.num_kmers;
  hash_table_print_stats(&db_graph.ht);

  size_t visited_words = roundup_bits2words64(db_graph.ht.capacity);
  uint64_t *visited = calloc2(visited_words, sizeof(uint64_t));
  // visited[0] is used to check if array is 'clean'

  if(len_before_fh != NULL) {
    // Save supernode lengths
    supernode_write_len_distrib(len_before_fh, len_before_path, LEN_HIST_CAP,
                                visited, &db_graph);
    visited[0] = 1;
    fclose(len_before_fh);
  }

  // Tip clipping
  if(tip_cleaning) {
    if(visited[0]) memset(visited, 0, visited_words * sizeof(uint64_t));
    cleaning_remove_tips(max_tip_len, visited, &db_graph);
    visited[0] = 1;
  }

  // Supernode cleaning or printing coverage distribution to a file
  if(supernode_cleaning || dump_covgs) {
    if(visited[0]) memset(visited, 0, visited_words * sizeof(uint64_t));

    threshold = cleaning_remove_supernodes(supernode_cleaning, threshold,
                                           seq_depth, dump_covgs,
                                           visited, &db_graph);
    visited[0] = 1;

    if(threshold == 0) {
      supernode_cleaning = false;
      doing_cleaning = (supernode_cleaning || tip_cleaning);
    }
  }

  if(len_after_fh != NULL) {
    // Save supernode lengths
    if(visited[0]) memset(visited, 0, visited_words * sizeof(uint64_t));

    supernode_write_len_distrib(len_after_fh, len_after_path, LEN_HIST_CAP,
                                visited, &db_graph);
    visited[0] = 1;
    fclose(len_after_fh);
  }

  free(visited);

  if(doing_cleaning)
  {
    // Output graph file
    Edges *intersect_edges = NULL;
    bool kmers_loaded = true;
    size_t thresh;

    // Set output header ginfo cleaned
    for(i = 0; i < total_cols; i++)
    {
      cleaning = &outhdr.ginfo[i].cleaning;
      cleaning->cleaned_snodes |= supernode_cleaning;
      cleaning->cleaned_tips |= tip_cleaning;

      if(supernode_cleaning) {
        thresh = cleaning->clean_snodes_thresh;
        thresh = cleaning->cleaned_snodes ? MIN2(thresh, threshold) : threshold;
        cleaning->clean_snodes_thresh = thresh;
      }
    }

    if(!all_colours_loaded)
    {
      // We haven't loaded all the colours
      // intersect_edges are edges to mask with
      // resets graph edges
      intersect_edges = db_graph.col_edges;
      db_graph.col_edges += db_graph.ht.capacity;
    }

    // Print stats on removed kmers
    size_t removed_nkmers = initial_nkmers - db_graph.ht.num_kmers;
    double removed_pct = (100.0 * removed_nkmers) / initial_nkmers;
    char removed_str[100], init_str[100];
    ulong_to_str(removed_nkmers, removed_str);
    ulong_to_str(initial_nkmers, init_str);
    status("Removed %s of %s (%.2f%%) kmers", removed_str, init_str, removed_pct);

    graph_files_merge(out_ctx_path, files, num_files,
                      kmers_loaded, all_colours_loaded,
                      intersect_edges, &outhdr, &db_graph);
  }

  ctx_check(db_graph.ht.num_kmers == hash_table_count_kmers(&db_graph.ht));

  graph_header_dealloc(&outhdr);

  free(edge_store);
  free(db_graph.col_covgs);
  db_graph_dealloc(&db_graph);

  for(i = 0; i < num_files; i++) graph_file_dealloc(&files[i]);

  return EXIT_SUCCESS;
}