/* MegaZeux
 *
 * Copyright (C) 2008 Alistair John Strachan <alistair@devzero.co.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "manifest.h"
#include "util.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define BLOCK_SIZE    4096UL
#define LINE_BUF_LEN  256

static bool manifest_parse_sha256(const char *p, uint32_t sha256[8])
{
  int i, j;

  // A SHA256 digest is a fixed length (prefix zeroes)
  if(strlen(p) != 8 * 8)
    return false;

  // Walk the sha256 "registers" and decompose 8byte hex chunks
  for (i = 0, j = 0; i < 8; i++, j += 8)
  {
    char reg_hex[9], *end_ptr;

    // Pinch an 8 byte hex block from source string
    memcpy(reg_hex, p + j, 8);
    reg_hex[8] = 0;

    // Convert the hex string into an integer and check for errors
    sha256[i] = (uint32_t)strtoul(reg_hex, &end_ptr, 16);
    if(end_ptr[0])
      return false;
  }

  return true;
}

static bool manifest_parse_size(char *p, unsigned long *size)
{
  char *endptr;
  *size = strtoul(p, &endptr, 10);
  if(endptr[0])
    return false;
  return true;
}

static void manifest_entry_free(struct manifest_entry *e)
{
  free(e->name);
  free(e);
}

void manifest_list_free(struct manifest_entry **head)
{
  struct manifest_entry *e, *next_e;
  for(e = *head; e; e = next_e)
  {
    next_e = e->next;
    manifest_entry_free(e);
  }
  *head = NULL;
}

static struct manifest_entry *manifest_entry_copy(struct manifest_entry *src)
{
  struct manifest_entry *dest;
  size_t name_len;

  dest = malloc(sizeof(struct manifest_entry));

  name_len = strlen(src->name);
  dest->name = malloc(name_len + 1);
  strncpy(dest->name, src->name, name_len);
  dest->name[name_len] = 0;

  memcpy(dest->sha256, src->sha256, sizeof(uint32_t) * 8);
  dest->size = src->size;
  dest->next = NULL;

  return dest;
}

static struct manifest_entry *manifest_list_copy(struct manifest_entry *src)
{
  struct manifest_entry *src_e, *dest = NULL, *dest_e, *next_dest_e;

  for(src_e = src; src_e; src_e = src_e->next)
  {
    next_dest_e = manifest_entry_copy(src_e);
    if(dest)
    {
      dest_e->next = next_dest_e;
      dest_e = dest_e->next;
    }
    else
      dest = dest_e = next_dest_e;
  }

  return dest;
}

static struct manifest_entry *manifest_list_create(FILE *f)
{
  struct manifest_entry *head = NULL, *e = NULL, *next_e;
  char buffer[LINE_BUF_LEN];

  // Walk the manifest line by line
  while(true)
  {
    char *m = buffer, *line;
    size_t line_len;

    // Grab a single line from the manifest
    if(!fgets(buffer, LINE_BUF_LEN, f))
      break;

    next_e = calloc(1, sizeof(struct manifest_entry));
    if(head)
    {
      e->next = next_e;
      e = e->next;
    }
    else
      head = e = next_e;

    // Grab the sha256 token
    line = strsep(&m, " ");
    if(!line)
      goto err_invalid_manifest;

    /* Break the 64 character (8x8) hex string down into
     * eight constituent 32bit SHA result registers (32*8=256).
     */
    if(!manifest_parse_sha256(line, e->sha256))
      goto err_invalid_manifest;

    // Grab the size token
    line = strsep(&m, " ");
    if(!line)
      goto err_invalid_manifest;

    // Validate and parse it
    if(!manifest_parse_size(line, &e->size))
      goto err_invalid_manifest;

    // Grab the filename token
    line = strsep(&m, "\n");
    if(!line)
      goto err_invalid_manifest;

    line_len = strlen(line);
    e->name = malloc(line_len + 1);
    strncpy(e->name, line, line_len);
    e->name[line_len] = 0;
  }

  if(e)
    e->next = NULL;

  return head;

err_invalid_manifest:
  warning("Malformed manifest file.\n");
  manifest_list_free(&head);
  return head;
}

static struct manifest_entry *manifest_get_local(void)
{
  struct manifest_entry *manifest;
  FILE *f;

  f = fopen("manifest.txt", "rb");
  if(!f)
  {
    warning("Local manifest.txt is missing\n");
    return NULL;
  }

  manifest = manifest_list_create(f);
  fclose(f);

  return manifest;
}

static struct manifest_entry *manifest_get_remote(struct host *h,
 const char *base)
{
  struct manifest_entry *manifest;
  char url[LINE_BUF_LEN];
  host_status_t ret;
  FILE *f;

  snprintf(url, LINE_BUF_LEN, "%s/manifest.txt", base);

  f = fopen("manifest.txt", "w+b");
  if(!f)
  {
    warning("Failed to open local manifest.txt for writing\n");
    return NULL;
  }

  ret = host_recv_file(h, url, f, "text/plain");
  if(ret != HOST_SUCCESS)
  {
    warning("Processing manifest.txt failed (error %d)\n", ret);
    return NULL;
  }

  rewind(f);
  manifest = manifest_list_create(f);
  fclose(f);

  return manifest;
}

static bool manifest_entry_equal(struct manifest_entry *l,
 struct manifest_entry *r)
{
  return strcmp(l->name, r->name) == 0 &&
         l->size == r->size &&
         memcmp(l->sha256, r->sha256, sizeof(uint32_t) * 8) == 0;
}

static void manifest_lists_remove_duplicates(struct manifest_entry **local,
 struct manifest_entry **remote)
{
  struct manifest_entry *l, *prev_l, *next_l;

  for(l = *local, prev_l = NULL; l; l = next_l)
  {
    struct manifest_entry *r, *prev_r, *next_r;

    for(r = *remote, prev_r = NULL; r; r = next_r)
    {
      next_r = r->next;

      if(manifest_entry_equal(l, r))
      {
        if(prev_r)
          prev_r->next = next_r;
        else
          *remote = next_r;
        manifest_entry_free(r);
        break;
      }
      else
        prev_r = r;
    }

    next_l = l->next;

    if(r)
    {
      if(prev_l)
        prev_l->next = next_l;
      else
        *local = next_l;
      manifest_entry_free(l);
    }
    else
      prev_l = l;
  }
}

static bool manifest_entry_check_validity(struct manifest_entry *e, FILE *f)
{
  unsigned long len = e->size, pos = 0;
  SHA256_ctx ctx;

  // It must be the same length
  if((unsigned long)ftell_and_rewind(f) != len)
    return false;

  /* Compute the SHA256 digest for this file. Do it block-wise so as to
   * conserve RAM and scale to enormously large files.
   */

  SHA256_init(&ctx);

  while(pos < len)
  {
    unsigned long block_size = MIN(BLOCK_SIZE, len - pos);
    char block[BLOCK_SIZE];

    if(fread(block, block_size, 1, f) != 1)
      return false;

    SHA256_update(&ctx, block, block_size);
    pos += block_size;
  }

  SHA256_final(&ctx);

  // Verify the digest against the manifest
  if(memcmp(ctx.H, e->sha256, sizeof(uint32_t) * 8) != 0)
    return false;

  return true;
}

static void manifest_add_list_validate_augment(struct manifest_entry *local,
 struct manifest_entry **added)
{
  struct manifest_entry *e, *a = *added;

  // Scan along to penultimate `added' (if we have any)
  if(a)
    while(a->next)
      a = a->next;

  for(e = local; e; e = e->next)
  {
    struct manifest_entry *new_added;
    FILE *f;

    f = fopen(e->name, "rb");
    if(f)
    {
      if(manifest_entry_check_validity(e, f))
      {
        fclose(f);
        continue;
      }
      fclose(f);
    }

    warning("Local file '%s' failed manifest validation\n", e->name);

    new_added = manifest_entry_copy(e);
    if(*added)
    {
      a->next = new_added;
      a = a->next;
    }
    else
      a = *added = new_added;
  }
}

bool manifest_get_updates(struct host *h, const char *basedir,
 struct manifest_entry **removed, struct manifest_entry **replaced,
 struct manifest_entry **added)
{
  struct manifest_entry *local, *remote;

  local = manifest_get_local();

  remote = manifest_get_remote(h, basedir);
  if(!remote)
    return false;

  *replaced = NULL;
  *removed = NULL;
  *added = NULL;

  if(local)
  {
    struct manifest_entry *added_copy;

    // Copy remote -> added because the list is modified by the next call
    *added = manifest_list_copy(remote);
    *removed = local;

    /* The "removed" list is simply the local list; both lists are modified
     * in place to filter any duplicate entries.
     */
    manifest_lists_remove_duplicates(removed, added);

    /* This hack removes the "added" list entries from the remote list, to
     * give us a list containing only the files that remained the same.
     */
    added_copy = manifest_list_copy(*added);
    manifest_lists_remove_duplicates(&remote, &added_copy);
    assert(added_copy == NULL);
  }

  /* Now we've decided what files should be added/removed, we use the remote
   * subset manifest to produce a replacement list. Any file that doesn't match
   * the remote manifest is added to this third list.
   */
  manifest_add_list_validate_augment(remote, replaced);
  manifest_list_free(&remote);
  return true;
}

bool manifest_entry_download_replace(struct host *h, const char *basedir,
 struct manifest_entry *e)
{
  char buf[LINE_BUF_LEN];
  host_status_t ret;
  bool valid = false;
  FILE *f;

  /* Try to open our target file. If we can't open it, it might be
   * write protected or in-use. In this case, it may be possible to
   * rename the original file out of the way. Try this trick first.
   */
  f = fopen(e->name, "w+b");
  if(!f)
  {
    snprintf(buf, LINE_BUF_LEN, "%s~", e->name);
    if(rename(e->name, buf))
    {
      warning("Failed to rename in-use file '%s' to '%s'\n", e->name, buf);
      goto err_out;
    }

    f = fopen(e->name, "w+b");
    if(!f)
    {
      warning("Unable to open file '%s' for writing\n", e->name);
      goto err_out;
    }
  }

  snprintf(buf, LINE_BUF_LEN, "%s/%08x%08x%08x%08x%08x%08x%08x%08x", basedir,
    e->sha256[0], e->sha256[1], e->sha256[2], e->sha256[3],
    e->sha256[4], e->sha256[5], e->sha256[6], e->sha256[7]);

  ret = host_recv_file(h, buf, f, "application/octet-stream");
  if(ret != HOST_SUCCESS)
  {
    warning("File '%s' could not be downloaded (error %d)\n", e->name, ret);
    goto err_close;
  }

  rewind(f);
  if(!manifest_entry_check_validity(e, f))
  {
    warning("File '%s' failed validation\n", e->name);
    goto err_close;
  }

  valid = true;
err_close:
  fclose(f);
err_out:
  return valid;
}
