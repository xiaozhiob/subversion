/* stats.c -- implements the svn_fs_fs__get_stats private API.
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include "svn_dirent_uri.h"
#include "svn_fs.h"
#include "svn_pools.h"
#include "svn_sorts.h"

#include "private/svn_cache.h"
#include "private/svn_sorts_private.h"
#include "private/svn_string_private.h"
#include "private/svn_fs_fs_private.h"

#include "index.h"
#include "pack.h"
#include "rev_file.h"
#include "util.h"
#include "fs_fs.h"
#include "cached_data.h"
#include "low_level.h"

#include "../libsvn_fs/fs-loader.h"

#include "svn_private_config.h"

/* We group representations into 2x2 different kinds plus one default:
 * [dir / file] x [text / prop]. The assignment is done by the first node
 * that references the respective representation.
 */
typedef enum rep_kind_t
{
  /* The representation is not used _directly_, i.e. not referenced by any
   * noderev. However, some other representation may use it as delta base.
   * Null value. Should not occur in real-word repositories. */
  unused_rep,

  /* a properties on directory rep  */
  dir_property_rep,

  /* a properties on file rep  */
  file_property_rep,

  /* a directory rep  */
  dir_rep,

  /* a file rep  */
  file_rep
} rep_kind_t;

/* A representation fragment.
 */
typedef struct rep_stats_t
{
  /* absolute offset in the file */
  apr_off_t offset;

  /* item length in bytes */
  apr_size_t size;

  /* item length after de-deltification */
  apr_size_t expanded_size;

  /* revision that contains this representation
   * (may be referenced by other revisions, though) */
  svn_revnum_t revision;

  /* number of nodes that reference this representation */
  apr_uint32_t ref_count;

  /* length of the PLAIN / DELTA line in the source file in bytes */
  apr_uint16_t header_size;

  /* classification of the representation. values of rep_kind_t */
  char kind;

} rep_stats_t;

/* Represents a single revision.
 * There will be only one instance per revision. */
typedef struct revision_info_t
{
  /* number of this revision */
  svn_revnum_t revision;

  /* pack file offset (manifest value), 0 for non-packed files */
  apr_off_t offset;

  /* offset of the changes list relative to OFFSET */
  apr_size_t changes;

  /* length of the changes list on bytes */
  apr_size_t changes_len;

  /* offset of the changes list relative to OFFSET */
  apr_size_t change_count;

  /* first offset behind the revision data in the pack file (file length
   * for non-packed revs) */
  apr_off_t end;

  /* number of directory noderevs in this revision */
  apr_size_t dir_noderev_count;

  /* number of file noderevs in this revision */
  apr_size_t file_noderev_count;

  /* total size of directory noderevs (i.e. the structs - not the rep) */
  apr_size_t dir_noderev_size;

  /* total size of file noderevs (i.e. the structs - not the rep) */
  apr_size_t file_noderev_size;

  /* all rep_stats_t of this revision (in no particular order),
   * i.e. those that point back to this struct */
  apr_array_header_t *representations;

  /* Temporary rev / pack file access object, used in phys. addressing
   * mode only.  NULL when done reading this revision. */
  svn_fs_fs__revision_file_t *rev_file;
} revision_info_t;

/* Root data structure containing all information about a given repository.
 * We use it as a wrapper around svn_fs_t and pass it around where we would
 * otherwise just use a svn_fs_t.
 */
typedef struct query_t
{
  /* FS API object*/
  svn_fs_t *fs;

  /* The HEAD revision. */
  svn_revnum_t head;

  /* Number of revs per shard; 0 for non-sharded repos. */
  int shard_size;

  /* First non-packed revision. */
  svn_revnum_t min_unpacked_rev;

  /* all revisions */
  apr_array_header_t *revisions;

  /* empty representation.
   * Used as a dummy base for DELTA reps without base. */
  rep_stats_t *null_base;

  /* collected statistics */
  svn_fs_fs__stats_t *stats;

  /* Progress notification callback to call after each shard.  May be NULL. */
  svn_fs_progress_notify_func_t progress_func;

  /* Baton for PROGRESS_FUNC. */
  void *progress_baton;

  /* Cancellation support callback to call once in a while.  May be NULL. */
  svn_cancel_func_t cancel_func;

  /* Baton for CANCEL_FUNC. */
  void *cancel_baton;
} query_t;

/* Return the length of REV_FILE in *FILE_SIZE.  Use POOL for allocations.
*/
static svn_error_t *
get_file_size(apr_off_t *file_size,
              svn_fs_fs__revision_file_t *rev_file,
              apr_pool_t *pool)
{
  apr_finfo_t finfo;

  SVN_ERR(svn_io_file_info_get(&finfo, APR_FINFO_SIZE, rev_file->file,
                               pool));

  *file_size = finfo.size;
  return SVN_NO_ERROR;
}

/* Get the file content of revision REVISION in QUERY and return it in
 * *CONTENT.  Read the LEN bytes starting at file OFFSET.  When provided,
 * use FILE as packed or plain rev file.
 * Use POOL for temporary allocations.
 */
static svn_error_t *
get_content(svn_stringbuf_t **content,
            apr_file_t *file,
            query_t *query,
            svn_revnum_t revision,
            apr_off_t offset,
            apr_size_t len,
            apr_pool_t *pool)
{
  apr_pool_t * file_pool = svn_pool_create(pool);
  apr_size_t large_buffer_size = 0x10000;

  *content = svn_stringbuf_create_ensure(len, pool);
  (*content)->len = len;

  /* for better efficiency use larger buffers on large reads */
  if (   (len >= large_buffer_size)
      && (apr_file_buffer_size_get(file) < large_buffer_size))
    apr_file_buffer_set(file,
                        apr_palloc(apr_file_pool_get(file),
                                   large_buffer_size),
                        large_buffer_size);

  SVN_ERR(svn_io_file_seek(file, APR_SET, &offset, pool));
  SVN_ERR(svn_io_file_read_full2(file, (*content)->data, len,
                                 NULL, NULL, pool));
  svn_pool_destroy(file_pool);

  return SVN_NO_ERROR;
}

/* Initialize the LARGEST_CHANGES member in STATS with a capacity of COUNT
 * entries.  Use POOL for allocations.
 */
static void
initialize_largest_changes(svn_fs_fs__stats_t *stats,
                           apr_size_t count,
                           apr_pool_t *pool)
{
  apr_size_t i;

  stats->largest_changes = apr_pcalloc(pool, sizeof(*stats->largest_changes));
  stats->largest_changes->count = count;
  stats->largest_changes->min_size = 1;
  stats->largest_changes->changes
    = apr_palloc(pool, count * sizeof(*stats->largest_changes->changes));

  /* allocate *all* entries before the path stringbufs.  This increases
   * cache locality and enhances performance significantly. */
  for (i = 0; i < count; ++i)
    stats->largest_changes->changes[i]
      = apr_palloc(pool, sizeof(**stats->largest_changes->changes));

  /* now initialize them and allocate the stringbufs */
  for (i = 0; i < count; ++i)
    {
      stats->largest_changes->changes[i]->size = 0;
      stats->largest_changes->changes[i]->revision = SVN_INVALID_REVNUM;
      stats->largest_changes->changes[i]->path
        = svn_stringbuf_create_ensure(1024, pool);
    }
}

/* Add entry for SIZE to HISTOGRAM.
 */
static void
add_to_histogram(svn_fs_fs__histogram_t *histogram,
                 apr_int64_t size)
{
  apr_int64_t shift = 0;

  while (((apr_int64_t)(1) << shift) <= size)
    shift++;

  histogram->total.count++;
  histogram->total.sum += size;
  histogram->lines[(apr_size_t)shift].count++;
  histogram->lines[(apr_size_t)shift].sum += size;
}

/* Update data aggregators in STATS with this representation of type KIND,
 * on-disk REP_SIZE and expanded node size EXPANDED_SIZE for PATH in REVSION.
 * PLAIN_ADDED indicates whether the node has a deltification predecessor.
 */
static void
add_change(svn_fs_fs__stats_t *stats,
           apr_int64_t rep_size,
           apr_int64_t expanded_size,
           svn_revnum_t revision,
           const char *path,
           rep_kind_t kind,
           svn_boolean_t plain_added)
{
  /* identify largest reps */
  if (rep_size >= stats->largest_changes->min_size)
    {
      apr_size_t i;
      svn_fs_fs__largest_changes_t *largest_changes = stats->largest_changes;
      svn_fs_fs__large_change_info_t *info
        = largest_changes->changes[largest_changes->count - 1];
      info->size = rep_size;
      info->revision = revision;
      svn_stringbuf_set(info->path, path);

      /* linear insertion but not too bad since count is low and insertions
       * near the end are more likely than close to front */
      for (i = largest_changes->count - 1; i > 0; --i)
        if (largest_changes->changes[i-1]->size >= rep_size)
          break;
        else
          largest_changes->changes[i] = largest_changes->changes[i-1];

      largest_changes->changes[i] = info;
      largest_changes->min_size
        = largest_changes->changes[largest_changes->count-1]->size;
    }

  /* global histograms */
  add_to_histogram(&stats->rep_size_histogram, rep_size);
  add_to_histogram(&stats->node_size_histogram, expanded_size);

  if (plain_added)
    {
      add_to_histogram(&stats->added_rep_size_histogram, rep_size);
      add_to_histogram(&stats->added_node_size_histogram, expanded_size);
    }

  /* specific histograms by type */
  switch (kind)
    {
      case unused_rep:        add_to_histogram(&stats->unused_rep_histogram,
                                               rep_size);
                              break;
      case dir_property_rep:  add_to_histogram(&stats->dir_prop_rep_histogram,
                                               rep_size);
                              add_to_histogram(&stats->dir_prop_histogram,
                                              expanded_size);
                              break;
      case file_property_rep: add_to_histogram(&stats->file_prop_rep_histogram,
                                               rep_size);
                              add_to_histogram(&stats->file_prop_histogram,
                                               expanded_size);
                              break;
      case dir_rep:           add_to_histogram(&stats->dir_rep_histogram,
                                               rep_size);
                              add_to_histogram(&stats->dir_histogram,
                                               expanded_size);
                              break;
      case file_rep:          add_to_histogram(&stats->file_rep_histogram,
                                               rep_size);
                              add_to_histogram(&stats->file_histogram,
                                               expanded_size);
                              break;
    }

  /* by extension */
  if (kind == file_rep)
    {
      /* determine extension */
      svn_fs_fs__extension_info_t *info;
      const char * file_name = strrchr(path, '/');
      const char * extension = file_name ? strrchr(file_name, '.') : NULL;

      if (extension == NULL || extension == file_name + 1)
        extension = "(none)";

      /* get / auto-insert entry for this extension */
      info = apr_hash_get(stats->by_extension, extension, APR_HASH_KEY_STRING);
      if (info == NULL)
        {
          apr_pool_t *pool = apr_hash_pool_get(stats->by_extension);
          info = apr_pcalloc(pool, sizeof(*info));
          info->extension = apr_pstrdup(pool, extension);

          apr_hash_set(stats->by_extension, info->extension,
                       APR_HASH_KEY_STRING, info);
        }

      /* update per-extension histogram */
      add_to_histogram(&info->node_histogram, expanded_size);
      add_to_histogram(&info->rep_histogram, rep_size);
    }
}

/* Read header information for the revision stored in FILE_CONTENT (one
 * whole revision).  Return the offsets within FILE_CONTENT for the
 * *ROOT_NODEREV, the list of *CHANGES and its len in *CHANGES_LEN.
 * Use POOL for temporary allocations. */
static svn_error_t *
read_revision_header(apr_size_t *changes,
                     apr_size_t *changes_len,
                     apr_size_t *root_noderev,
                     svn_stringbuf_t *file_content,
                     apr_pool_t *pool)
{
  char buf[64];
  const char *line;
  char *space;
  apr_uint64_t val;
  apr_size_t len;

  /* Read in this last block, from which we will identify the last line. */
  len = sizeof(buf);
  if (len > file_content->len)
    len = file_content->len;

  memcpy(buf, file_content->data + file_content->len - len, len);

  /* The last byte should be a newline. */
  if (buf[(apr_ssize_t)len - 1] != '\n')
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Revision lacks trailing newline"));

  /* Look for the next previous newline. */
  buf[len - 1] = 0;
  line = strrchr(buf, '\n');
  if (line == NULL)
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Final line in revision file longer "
                              "than 64 characters"));

  space = strchr(line, ' ');
  if (space == NULL)
    return svn_error_create(SVN_ERR_FS_CORRUPT, NULL,
                            _("Final line in revision file missing space"));

  /* terminate the header line */
  *space = 0;

  /* extract information */
  SVN_ERR(svn_cstring_strtoui64(&val, line+1, 0, APR_SIZE_MAX, 10));
  *root_noderev = (apr_size_t)val;
  SVN_ERR(svn_cstring_strtoui64(&val, space+1, 0, APR_SIZE_MAX, 10));
  *changes = (apr_size_t)val;
  *changes_len = file_content->len - *changes - (buf + len - line) + 1;

  return SVN_NO_ERROR;
}

/* Comparator used for binary search comparing the absolute file offset
 * of a representation to some other offset. DATA is a *rep_stats_t,
 * KEY is a pointer to an apr_off_t.
 */
static int
compare_representation_offsets(const void *data, const void *key)
{
  apr_off_t lhs = (*(const rep_stats_t *const *)data)->offset;
  apr_off_t rhs = *(const apr_off_t *)key;

  if (lhs < rhs)
    return -1;
  return (lhs > rhs ? 1 : 0);
}

/* Find the revision_info_t object to the given REVISION in QUERY and
 * return it in *REVISION_INFO. For performance reasons, we skip the
 * lookup if the info is already provided.
 *
 * In that revision, look for the rep_stats_t object for offset OFFSET.
 * If it already exists, set *IDX to its index in *REVISION_INFO's
 * representations list and return the representation object. Otherwise,
 * set the index to where it must be inserted and return NULL.
 */
static rep_stats_t *
find_representation(int *idx,
                    query_t *query,
                    revision_info_t **revision_info,
                    svn_revnum_t revision,
                    apr_off_t offset)
{
  revision_info_t *info;
  *idx = -1;

  /* first let's find the revision */
  info = revision_info ? *revision_info : NULL;
  if (info == NULL || info->revision != revision)
    {
      info = APR_ARRAY_IDX(query->revisions, revision, revision_info_t*);
      if (revision_info)
        *revision_info = info;
    }

  /* not found -> no result */
  if (info == NULL)
    return NULL;

  /* look for the representation */
  *idx = svn_sort__bsearch_lower_bound(info->representations,
                                       &offset,
                                       compare_representation_offsets);
  if (*idx < info->representations->nelts)
    {
      /* return the representation, if this is the one we were looking for */
      rep_stats_t *result
        = APR_ARRAY_IDX(info->representations, *idx, rep_stats_t *);
      if (result->offset == offset)
        return result;
    }

  /* not parsed, yet */
  return NULL;
}

/* Find / auto-construct the representation stats for REP in QUERY and
 * return it in *REPRESENTATION.
 *
 * If necessary, allocate the result in POOL; use SCRATCH_POOL for temp.
 * allocations.
 */
static svn_error_t *
parse_representation(rep_stats_t **representation,
                     query_t *query,
                     representation_t *rep,
                     revision_info_t *revision_info,
                     apr_pool_t *pool,
                     apr_pool_t *scratch_pool)
{
  rep_stats_t *result;
  int idx;

  /* read location (revision, offset) and size */

  /* look it up */
  result = find_representation(&idx, query, &revision_info, rep->revision,
                               (apr_off_t)rep->item_index);
  if (!result)
    {
      /* not parsed, yet (probably a rep in the same revision).
       * Create a new rep object and determine its base rep as well.
       */
      result = apr_pcalloc(pool, sizeof(*result));
      result->revision = rep->revision;
      result->expanded_size = (rep->expanded_size ? rep->expanded_size
                                                  : rep->size);
      result->offset = (apr_off_t)rep->item_index;
      result->size = rep->size;

      /* In phys. addressing mode, follow link to the actual representation.
       * In log. addressing mode, we will find it already as part of our
       * linear walk through the whole file. */
      if (!svn_fs_fs__use_log_addressing(query->fs))
        {
          svn_fs_fs__rep_header_t *header;
          apr_off_t offset = revision_info->offset + result->offset;

          SVN_ERR_ASSERT(revision_info->rev_file);
          SVN_ERR(svn_io_file_seek(revision_info->rev_file->file, APR_SET,
                                   &offset, scratch_pool));
          SVN_ERR(svn_fs_fs__read_rep_header(&header,
                                             revision_info->rev_file->stream,
                                             scratch_pool, scratch_pool));

          result->header_size = header->header_size;
        }

      svn_sort__array_insert(revision_info->representations, &result, idx);
    }

  *representation = result;

  return SVN_NO_ERROR;
}


/* forward declaration */
static svn_error_t *
read_noderev(query_t *query,
             svn_stringbuf_t *file_content,
             apr_size_t offset,
             revision_info_t *revision_info,
             apr_pool_t *pool,
             apr_pool_t *scratch_pool);

/* Starting at the directory in NODEREV's text in FILE_CONTENT, read all
 * DAG nodes, directories and representations linked in that tree structure.
 * Store them in QUERY and REVISION_INFO.  Also, read them only once.
 *
 * Use POOL for persistent allocations and SCRATCH_POOL for temporaries.
 */
static svn_error_t *
parse_dir(query_t *query,
          svn_stringbuf_t *file_content,
          node_revision_t *noderev,
          revision_info_t *revision_info,
          apr_pool_t *pool,
          apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_pool_t *subpool = svn_pool_create(scratch_pool);

  int i;
  apr_array_header_t *entries;
  SVN_ERR(svn_fs_fs__rep_contents_dir(&entries, query->fs, noderev,
                                      subpool, subpool));

  for (i = 0; i < entries->nelts; ++i)
    {
      svn_fs_dirent_t *dirent = APR_ARRAY_IDX(entries, i, svn_fs_dirent_t *);

      if (svn_fs_fs__id_rev(dirent->id) == revision_info->revision)
        {
          svn_pool_clear(iterpool);
          SVN_ERR(read_noderev(query, file_content,
                               (apr_size_t)svn_fs_fs__id_item(dirent->id),
                               revision_info, pool, iterpool));
        }
    }

  svn_pool_destroy(iterpool);
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

/* Starting at the noderev at OFFSET in FILE_CONTENT, read all DAG nodes,
 * directories and representations linked in that tree structure.  Store
 * them in QUERY and REVISION_INFO.  Also, read them only once.  Return the
 * result in *NODEREV.
 *
 * Use POOL for persistent allocations and SCRATCH_POOL for temporaries.
 */
static svn_error_t *
read_noderev(query_t *query,
             svn_stringbuf_t *file_content,
             apr_size_t offset,
             revision_info_t *revision_info,
             apr_pool_t *pool,
             apr_pool_t *scratch_pool)
{
  const char *end_marker = "\n\n";
  const char *noderev_end = strstr(file_content->data + offset, end_marker);
  apr_size_t noderev_len = noderev_end ? (noderev_end - file_content->data -
                                          offset + strlen(end_marker))
                                       : (file_content->len - offset);

  rep_stats_t *text = NULL;
  rep_stats_t *props = NULL;

  node_revision_t *noderev;
  apr_pool_t *subpool = svn_pool_create(scratch_pool);

  svn_stream_t *stream = svn_stream_from_stringbuf(file_content, subpool);
  svn_stream_skip(stream, offset);
  SVN_ERR(svn_fs_fs__read_noderev(&noderev, stream, subpool, subpool));

  if (noderev->data_rep)
    {
      SVN_ERR(parse_representation(&text, query,
                                   noderev->data_rep, revision_info,
                                   pool, subpool));

      /* if we are the first to use this rep, mark it as "text rep" */
      if (++text->ref_count == 1)
        text->kind = noderev->kind == svn_node_dir ? dir_rep : file_rep;
    }

  if (noderev->prop_rep)
    {
      SVN_ERR(parse_representation(&props, query,
                                   noderev->prop_rep, revision_info,
                                   pool, subpool));

      /* if we are the first to use this rep, mark it as "prop rep" */
      if (++props->ref_count == 1)
        props->kind = noderev->kind == svn_node_dir ? dir_property_rep
                                                    : file_property_rep;
    }

  /* record largest changes */
  if (text && text->ref_count == 1)
    add_change(query->stats, (apr_int64_t)text->size,
               (apr_int64_t)text->expanded_size, text->revision,
               noderev->created_path, text->kind, !noderev->predecessor_id);
  if (props && props->ref_count == 1)
    add_change(query->stats, (apr_int64_t)props->size,
               (apr_int64_t)props->expanded_size, props->revision,
               noderev->created_path, props->kind, !noderev->predecessor_id);

  /* if this is a directory and has not been processed, yet, read and
   * process it recursively */
  if (   noderev->kind == svn_node_dir && text && text->ref_count == 1
      && !svn_fs_fs__use_log_addressing(query->fs))
    SVN_ERR(parse_dir(query, file_content, noderev, revision_info,
                      pool, subpool));

  /* update stats */
  if (noderev->kind == svn_node_dir)
    {
      revision_info->dir_noderev_size += noderev_len;
      revision_info->dir_noderev_count++;
    }
  else
    {
      revision_info->file_noderev_size += noderev_len;
      revision_info->file_noderev_count++;
    }
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

/* Given the unparsed changes list in CHANGES with LEN chars, return the
 * number of changed paths encoded in it.
 */
static apr_size_t
get_change_count(const char *changes,
                 apr_size_t len)
{
  apr_size_t lines = 0;
  const char *end = changes + len;

  /* line count */
  for (; changes < end; ++changes)
    if (*changes == '\n')
      ++lines;

  /* two lines per change */
  return lines / 2;
}

/* Read the content of the pack file staring at revision BASE physical
 * addressing mode and store it in QUERY.  Use POOL for allocations.
 */
static svn_error_t *
read_phys_pack_file(query_t *query,
                    svn_revnum_t base,
                    apr_pool_t *pool)
{
  apr_pool_t *local_pool = svn_pool_create(pool);
  apr_pool_t *iterpool = svn_pool_create(local_pool);
  int i;
  apr_off_t file_size = 0;
  svn_fs_fs__revision_file_t *rev_file;

  SVN_ERR(svn_fs_fs__open_pack_or_rev_file(&rev_file, query->fs, base,
                                           pool, pool));
  SVN_ERR(get_file_size(&file_size, rev_file, local_pool));

  /* process each revision in the pack file */
  for (i = 0; i < query->shard_size; ++i)
    {
      apr_size_t root_node_offset;
      svn_stringbuf_t *rev_content;
      revision_info_t *info;

      /* cancellation support */
      if (query->cancel_func)
        SVN_ERR(query->cancel_func(query->cancel_baton));

      /* create the revision info for the current rev */
      info = apr_pcalloc(pool, sizeof(*info));
      info->representations = apr_array_make(iterpool, 4, sizeof(rep_stats_t*));
      info->rev_file = rev_file;

      info->revision = base + i;
      SVN_ERR(svn_fs_fs__get_packed_offset(&info->offset, query->fs, base + i,
                                           iterpool));
      if (i + 1 == query->shard_size)
        info->end = file_size;
      else
        SVN_ERR(svn_fs_fs__get_packed_offset(&info->end, query->fs,
                                             base + i + 1, iterpool));

      SVN_ERR(get_content(&rev_content, rev_file->file, query, info->revision,
                          info->offset,
                          info->end - info->offset,
                          iterpool));

      SVN_ERR(read_revision_header(&info->changes,
                                   &info->changes_len,
                                   &root_node_offset,
                                   rev_content,
                                   iterpool));

      info->change_count
        = get_change_count(rev_content->data + info->changes,
                           info->changes_len);
      SVN_ERR(read_noderev(query, rev_content,
                           root_node_offset, info, pool, iterpool));

      info->representations = apr_array_copy(pool, info->representations);

      /* Done with this revision. */
      SVN_ERR(svn_fs_fs__close_revision_file(rev_file));
      info->rev_file = NULL;

      /* put it into our container */
      APR_ARRAY_PUSH(query->revisions, revision_info_t*) = info;

      /* destroy temps */
      svn_pool_clear(iterpool);
    }

  /* one more pack file processed */
  if (query->progress_func)
    query->progress_func(base, query->progress_baton, local_pool);

  svn_pool_destroy(local_pool);

  return SVN_NO_ERROR;
}

/* Read the content of the file for REVISION in physical addressing mode
 * and store its contents in QUERY.  Use POOL for allocations.
 */
static svn_error_t *
read_phys_revision_file(query_t *query,
                        svn_revnum_t revision,
                        apr_pool_t *pool)
{
  apr_size_t root_node_offset;
  apr_pool_t *local_pool = svn_pool_create(pool);
  svn_stringbuf_t *rev_content;
  revision_info_t *info = apr_pcalloc(pool, sizeof(*info));
  apr_off_t file_size = 0;
  svn_fs_fs__revision_file_t *rev_file;

  /* cancellation support */
  if (query->cancel_func)
    SVN_ERR(query->cancel_func(query->cancel_baton));

  /* read the whole pack file into memory */
  SVN_ERR(svn_fs_fs__open_pack_or_rev_file(&rev_file, query->fs, revision,
                                           pool, pool));
  SVN_ERR(get_file_size(&file_size, rev_file, local_pool));

  /* create the revision info for the current rev */
  info->representations = apr_array_make(pool, 4, sizeof(rep_stats_t*));

  info->rev_file = rev_file;
  info->revision = revision;
  info->offset = 0;
  info->end = file_size;

  SVN_ERR(get_content(&rev_content, rev_file->file, query, revision, 0,
                      file_size, local_pool));

  SVN_ERR(read_revision_header(&info->changes,
                               &info->changes_len,
                               &root_node_offset,
                               rev_content,
                               local_pool));

  info->change_count
    = get_change_count(rev_content->data + info->changes,
                       info->changes_len);

  /* parse the revision content recursively. */
  SVN_ERR(read_noderev(query, rev_content,
                       root_node_offset, info,
                       pool, local_pool));

  /* Done with this revision. */
  SVN_ERR(svn_fs_fs__close_revision_file(rev_file));
  info->rev_file = NULL;

  /* put it into our container */
  APR_ARRAY_PUSH(query->revisions, revision_info_t*) = info;

  /* show progress every 1000 revs or so */
  if (query->progress_func)
    {
      if (query->shard_size && (revision % query->shard_size == 0))
        query->progress_func(revision, query->progress_baton, local_pool);
      if (!query->shard_size && (revision % 1000 == 0))
        query->progress_func(revision, query->progress_baton, local_pool);
    }

  svn_pool_destroy(local_pool);

  return SVN_NO_ERROR;
}

/* Read the item described by ENTRY from the REV_FILE and return
 * the respective byte sequence in *CONTENTS allocated in POOL.
 */
static svn_error_t *
read_item(svn_stringbuf_t **contents,
          svn_fs_fs__revision_file_t *rev_file,
          svn_fs_fs__p2l_entry_t *entry,
          apr_pool_t *pool)
{
  svn_stringbuf_t *item = svn_stringbuf_create_ensure(entry->size, pool);
  item->len = entry->size;
  item->data[item->len] = 0;

  SVN_ERR(svn_io_file_aligned_seek(rev_file->file, rev_file->block_size,
                                   NULL, entry->offset, pool));
  SVN_ERR(svn_io_file_read_full2(rev_file->file, item->data, item->len,
                                 NULL, NULL, pool));

  *contents = item;

  return SVN_NO_ERROR;
}

/* Process the logically addressed revision contents of revisions BASE to
 * BASE + COUNT - 1 in QUERY.  Use POOL for allocations.
 */
static svn_error_t *
read_log_rev_or_packfile(query_t *query,
                         svn_revnum_t base,
                         int count,
                         apr_pool_t *pool)
{
  fs_fs_data_t *ffd = query->fs->fsap_data;
  apr_pool_t *iterpool = svn_pool_create(pool);
  apr_pool_t *localpool = svn_pool_create(pool);
  apr_off_t max_offset;
  apr_off_t offset = 0;
  int i;
  svn_fs_fs__revision_file_t *rev_file;

  /* we will process every revision in the rev / pack file */
  for (i = 0; i < count; ++i)
    {
      /* create the revision info for the current rev */
      revision_info_t *info = apr_pcalloc(pool, sizeof(*info));
      info->representations = apr_array_make(pool, 4, sizeof(rep_stats_t*));
      info->revision = base + i;

      APR_ARRAY_PUSH(query->revisions, revision_info_t*) = info;
    }

  /* open the pack / rev file that is covered by the p2l index */
  SVN_ERR(svn_fs_fs__open_pack_or_rev_file(&rev_file, query->fs, base,
                                           localpool, iterpool));
  SVN_ERR(svn_fs_fs__p2l_get_max_offset(&max_offset, query->fs, rev_file,
                                        base, localpool));

  /* record the whole pack size in the first rev so the total sum will
     still be correct */
  APR_ARRAY_IDX(query->revisions, base, revision_info_t*)->end = max_offset;

  /* for all offsets in the file, get the P2L index entries and process
     the interesting items (change lists, noderevs) */
  for (offset = 0; offset < max_offset; )
    {
      apr_array_header_t *entries;

      svn_pool_clear(iterpool);

      /* cancellation support */
      if (query->cancel_func)
        SVN_ERR(query->cancel_func(query->cancel_baton));

      /* get all entries for the current block */
      SVN_ERR(svn_fs_fs__p2l_index_lookup(&entries, query->fs, rev_file, base,
                                          offset, ffd->p2l_page_size,
                                          iterpool, iterpool));

      /* process all entries (and later continue with the next block) */
      for (i = 0; i < entries->nelts; ++i)
        {
          svn_fs_fs__p2l_entry_t *entry
            = &APR_ARRAY_IDX(entries, i, svn_fs_fs__p2l_entry_t);

          /* skip bits we previously processed */
          if (i == 0 && entry->offset < offset)
            continue;

          /* skip zero-sized entries */
          if (entry->size == 0)
            continue;

          /* read and process interesting items */
          if (entry->type == SVN_FS_FS__ITEM_TYPE_NODEREV)
            {
              svn_stringbuf_t *item;
              revision_info_t *info = APR_ARRAY_IDX(query->revisions,
                                                    entry->item.revision,
                                                    revision_info_t*);
              SVN_ERR(read_item(&item, rev_file, entry, iterpool));
              SVN_ERR(read_noderev(query, item, 0, info, pool, iterpool));
            }
          else if (entry->type == SVN_FS_FS__ITEM_TYPE_CHANGES)
            {
              svn_stringbuf_t *item;
              revision_info_t *info = APR_ARRAY_IDX(query->revisions,
                                                    entry->item.revision,
                                                    revision_info_t*);
              SVN_ERR(read_item(&item, rev_file, entry, iterpool));
              info->change_count
                = get_change_count(item->data + 0, item->len);
              info->changes_len += entry->size;
            }

          /* advance offset */
          offset += entry->size;
        }
    }

  /* clean up and close file handles */
  svn_pool_destroy(iterpool);
  svn_pool_destroy(localpool);

  return SVN_NO_ERROR;
}

/* Read the content of the pack file staring at revision BASE logical
 * addressing mode and store it in QUERY.  Use POOL for allocations.
 */
static svn_error_t *
read_log_pack_file(query_t *query,
                   svn_revnum_t base,
                   apr_pool_t *pool)
{
  SVN_ERR(read_log_rev_or_packfile(query, base, query->shard_size, pool));

  /* one more pack file processed */
  if (query->progress_func)
    query->progress_func(base, query->progress_baton, pool);

  return SVN_NO_ERROR;
}

/* Read the content of the file for REVISION in logical addressing mode
 * and store its contents in QUERY.  Use POOL for allocations.
 */
static svn_error_t *
read_log_revision_file(query_t *query,
                       svn_revnum_t revision,
                       apr_pool_t *pool)
{
  SVN_ERR(read_log_rev_or_packfile(query, revision, 1, pool));

  /* show progress every 1000 revs or so */
  if (query->progress_func)
    {
      if (query->shard_size && (revision % query->shard_size == 0))
        query->progress_func(revision, query->progress_baton, pool);
      if (!query->shard_size && (revision % 1000 == 0))
        query->progress_func(revision, query->progress_baton, pool);
    }

  return SVN_NO_ERROR;
}

/* Read the repository and collect the stats info in QUERY.
 * Use POOL for allocations.
 */
static svn_error_t *
read_revisions(query_t *query,
               apr_pool_t *pool)
{
  svn_revnum_t revision;

  /* read all packed revs */
  for ( revision = 0
      ; revision < query->min_unpacked_rev
      ; revision += query->shard_size)
    if (svn_fs_fs__use_log_addressing(query->fs))
      SVN_ERR(read_log_pack_file(query, revision, pool));
    else
      SVN_ERR(read_phys_pack_file(query, revision, pool));

  /* read non-packed revs */
  for ( ; revision <= query->head; ++revision)
    if (svn_fs_fs__use_log_addressing(query->fs))
      SVN_ERR(read_log_revision_file(query, revision, pool));
    else
      SVN_ERR(read_phys_revision_file(query, revision, pool));

  return SVN_NO_ERROR;
}

/* Accumulate stats of REP in STATS.
 */
static void
add_rep_pack_stats(svn_fs_fs__rep_pack_stats_t *stats,
                   rep_stats_t *rep)
{
  stats->count++;

  stats->packed_size += rep->size;
  stats->expanded_size += rep->expanded_size;
  stats->overhead_size += rep->header_size + 7 /* ENDREP\n */;
}

/* Accumulate stats of REP in STATS.
 */
static void
add_rep_stats(svn_fs_fs__representation_stats_t *stats,
              rep_stats_t *rep)
{
  add_rep_pack_stats(&stats->total, rep);
  if (rep->ref_count == 1)
    add_rep_pack_stats(&stats->uniques, rep);
  else
    add_rep_pack_stats(&stats->shared, rep);

  stats->references += rep->ref_count;
  stats->expanded_size += rep->ref_count * rep->expanded_size;
}

/* Aggregate the info the in revision_info_t * array REVISIONS into the
 * respectve fields of STATS.
 */
static void
aggregate_stats(const apr_array_header_t *revisions,
                svn_fs_fs__stats_t *stats)
{
  int i, k;

  /* aggregate info from all revisions */
  stats->revision_count = revisions->nelts;
  for (i = 0; i < revisions->nelts; ++i)
    {
      revision_info_t *revision = APR_ARRAY_IDX(revisions, i,
                                                revision_info_t *);

      /* data gathered on a revision level */
      stats->change_count += revision->change_count;
      stats->change_len += revision->changes_len;
      stats->total_size += revision->end - revision->offset;

      stats->dir_node_stats.count += revision->dir_noderev_count;
      stats->dir_node_stats.size += revision->dir_noderev_size;
      stats->file_node_stats.count += revision->file_noderev_count;
      stats->file_node_stats.size += revision->file_noderev_size;
      stats->total_node_stats.count += revision->dir_noderev_count
                                    + revision->file_noderev_count;
      stats->total_node_stats.size += revision->dir_noderev_size
                                   + revision->file_noderev_size;

      /* process representations */
      for (k = 0; k < revision->representations->nelts; ++k)
        {
          rep_stats_t *rep = APR_ARRAY_IDX(revision->representations, k,
                                           rep_stats_t *);

          /* accumulate in the right bucket */
          switch(rep->kind)
            {
              case file_rep:
                add_rep_stats(&stats->file_rep_stats, rep);
                break;
              case dir_rep:
                add_rep_stats(&stats->dir_rep_stats, rep);
                break;
              case file_property_rep:
                add_rep_stats(&stats->file_prop_rep_stats, rep);
                break;
              case dir_property_rep:
                add_rep_stats(&stats->dir_prop_rep_stats, rep);
                break;
              default:
                break;
            }

          add_rep_stats(&stats->total_rep_stats, rep);
        }
    }
}

/* Return a new svn_fs_fs__stats_t instance, allocated in RESULT_POOL.
 */
static svn_fs_fs__stats_t *
create_stats(apr_pool_t *result_pool)
{
  svn_fs_fs__stats_t *stats = apr_pcalloc(result_pool, sizeof(*stats));

  initialize_largest_changes(stats, 64, result_pool);
  stats->by_extension = apr_hash_make(result_pool);

  return stats;
}

/* Create a *QUERY, allocated in RESULT_POOL, reading filesystem FS and
 * collecting results in STATS.  Store the optional PROCESS_FUNC and
 * PROGRESS_BATON as well as CANCEL_FUNC and CANCEL_BATON in *QUERY, too.
 * Use SCRATCH_POOL for temporary allocations.
 */
static svn_error_t *
create_query(query_t **query,
             svn_fs_t *fs,
             svn_fs_fs__stats_t *stats,
             svn_fs_progress_notify_func_t progress_func,
             void *progress_baton,
             svn_cancel_func_t cancel_func,
             void *cancel_baton,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  *query = apr_pcalloc(result_pool, sizeof(**query));

  /* Read repository dimensions. */
  (*query)->shard_size = svn_fs_fs__shard_size(fs);
  SVN_ERR(svn_fs_fs__youngest_rev(&(*query)->head, fs, scratch_pool));
  SVN_ERR(svn_fs_fs__min_unpacked_rev(&(*query)->min_unpacked_rev, fs,
                                      scratch_pool));

  /* create data containers and caches
   * Note: this assumes that int is at least 32-bits and that we only support
   * 32-bit wide revision numbers (actually 31-bits due to the signedness
   * of both the nelts field of the array and our revision numbers). This
   * means this code will fail on platforms where int is less than 32-bits
   * and the repository has more revisions than int can hold. */
  (*query)->revisions = apr_array_make(result_pool, (int) (*query)->head + 1,
                                       sizeof(revision_info_t *));
  (*query)->null_base = apr_pcalloc(result_pool,
                                    sizeof(*(*query)->null_base));

  /* Store other parameters */
  (*query)->fs = fs;
  (*query)->stats = stats;
  (*query)->progress_func = progress_func;
  (*query)->progress_baton = progress_baton;
  (*query)->cancel_func = cancel_func;
  (*query)->cancel_baton = cancel_baton;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_fs__get_stats(svn_fs_fs__stats_t **stats,
                     svn_fs_t *fs,
                     svn_fs_progress_notify_func_t progress_func,
                     void *progress_baton,
                     svn_cancel_func_t cancel_func,
                     void *cancel_baton,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  query_t *query;

  *stats = create_stats(result_pool);
  SVN_ERR(create_query(&query, fs, *stats, progress_func, progress_func,
                       cancel_func, cancel_baton, scratch_pool,
                       scratch_pool));
  SVN_ERR(read_revisions(query, scratch_pool));
  aggregate_stats(query->revisions, *stats);

  return SVN_NO_ERROR;
}
