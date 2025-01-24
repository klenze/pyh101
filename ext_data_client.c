/* This file is part of UCESB - a tool for data unpacking and processing.
 *
 * Copyright (C) 2016  Haakan T. Johansson  <f96hajo@chalmers.se>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301  USA
 */

#define EXT_DATA_CLIENT_INTERNALS
#include "ext_data_client.h"
#include "ext_data_proto.h"

/* #include "ext_file_writer.hh" */

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <assert.h>
#include <fcntl.h>

#include <stdio.h>

// For NetBSD, the error EPROTO does not exist, so use something else
#ifndef EPROTO
#define EPROTO EBADMSG
#endif

/* gcc 4.3 and later cannot figure out that code in glibc bits/byteswap.h
 * does _not_ get hit by 'conversion to short int from int may alter its
 * value', so we have a local copy:
 * looks like at least 4.6 copes, so disabling again
 */
#ifdef __GNUC_PREREQ
# if __GNUC_PREREQ (4, 3) && !__GNUC_PREREQ (4, 6)
#  undef bswap_16
#  define __local_bswap_16(x)			\
  (unsigned short int) ((((x) >> 8) & 0x00ff) | \
                        (((x) << 8) & 0xff00))
#  define bswap_16(x)   __local_bswap_16(x)
#  ifdef ntohs
#   undef ntohs
#   define ntohs(x)     __local_bswap_16 (x)
#  endif
#  ifdef htons
#   undef htons
#   define htons(x)     __local_bswap_16 (x)
#  endif
# endif
#endif

#if !USING_CERNLIB && !USING_ROOT &&!STRUCT_WRITER
#define PURE_EXT_DATA_CLIENT 1
#endif

struct ext_data_structure_info
{
  struct ext_data_structure_item *_items;

  /* Used while returning items for ext_data_struct_info_map_success(). */
  struct ext_data_structure_item *_ret_item;
  int         _ret_for_server;

  struct ext_data_structure_info *_server_struct_info;

  uint32_t    _map_success;

  const char *_last_error;
};

struct ext_data_client_struct
{
  const char *_id;

  /* Todo: the following controls _raw_ptr in ext_data_client.  With
   * several structures...?
   */
  uint32_t  _max_raw_words;

  uint32_t  _orig_xor_sum_msg;
  size_t    _orig_struct_size;
  void     *_orig_array; /* for mapping */

  uint32_t  _orig_max_pack_items;
  uint32_t  _orig_static_pack_items;

  uint32_t *_orig_pack_list;
  uint32_t *_orig_pack_list_end;

  size_t    _dest_struct_size;

  uint32_t  _dest_max_pack_items;
  uint32_t  _dest_static_pack_items;

  uint32_t *_dest_pack_list;
  uint32_t *_dest_pack_list_end;

  uint32_t *_dest_reverse_pack;

  uint32_t *_map_list;
  uint32_t *_map_list_end;

  struct ext_data_structure_info *_struct_info_msg;
};

#define EXT_DATA_STATE_INIT            0
#define EXT_DATA_STATE_OPEN            1 /* for read */
#define EXT_DATA_STATE_OPEN_OUT        2 /* for write */
#define EXT_DATA_STATE_PARSED_HEADERS  3
#define EXT_DATA_STATE_SETUP_READ      4
#define EXT_DATA_STATE_SETUP_WRITE     5

struct ext_data_client
{
  int _fd;
  int _fd_close; /* If using STDIN, we are not to close it, so -1. */

  char  *_buf;
  size_t _buf_alloc;
  size_t _buf_used;
  size_t _buf_filled;

  uint32_t *_raw_ptr;  /* This is not allocated; just used. */
  uint32_t  _raw_words;
  uint32_t *_raw_swapped;

  struct ext_data_client_struct *_structures;
  int                        _num_structures;

  uint32_t _sort_u32_words;

  const char *_last_error;

  int _state;

  int _fetched_event;
};

/* Layout of the structure information generated.
 */

struct ext_data_structure_layout_item
{
  uint32_t    _offset;
  uint32_t    _size;
  uint32_t    _xor;
  const char *_name;
};

struct ext_data_structure_layout
{
  uint32_t _magic;
  uint32_t _size_info;
  uint32_t _size_struct;
  uint32_t _size_struct_onion;
  uint32_t _pack_list_items;

  uint32_t _num_items;
  struct ext_data_structure_layout_item _items[1]; /* more than 1 for parts */
};

struct ext_data_structure_info *ext_data_struct_info_alloc()
{
  struct ext_data_structure_info *struct_info;

  struct_info = (struct ext_data_structure_info *)
    malloc (sizeof (struct ext_data_structure_info));

  if (!struct_info)
    {
      errno = ENOMEM;
      return NULL;
    }

  struct_info->_items = NULL;

  struct_info->_map_success = (uint32_t) -1;

  struct_info->_ret_item = NULL;
  struct_info->_ret_for_server = 0;

  struct_info->_server_struct_info = NULL;

  struct_info->_last_error = NULL;

  return struct_info;
}


struct ext_data_structure_item* ext_data_struct_info_get_items(struct ext_data_structure_info * info)
{
  if (!info)
    return NULL;
  return info->_items;
}

void ext_data_struct_info_free(struct ext_data_structure_info *struct_info)
{
  struct ext_data_structure_item *item;

  if (!struct_info)
    return;

  item = struct_info->_items;

  while (item)
    {
      struct ext_data_structure_item *fi = item;

      free((char *) item->_block);
      free((char *) item->_var_name);
      free((char *) item->_var_ctrl_name);

      item = item->_next_off_item;
      free(fi);
    }

  free(struct_info);
}

const char *
ext_data_struct_info_last_error(struct ext_data_structure_info *struct_info)
{
  if (struct_info == NULL)
    return NULL;

  return struct_info->_last_error;
}

#define DEBUG_MATCHING(...) do { } while (0)
/*efine DEBUG_MATCHING(...) do { fprintf (stderr,__VA_ARGS__); } while (0)*/

int
ext_data_struct_info_map_success(struct ext_data_structure_info *struct_info,
				 int restart,
				 const char **var_name,
				 uint32_t *offset,
				 uint32_t *map_success)
{
  struct ext_data_structure_item *item;
  
  if (!struct_info)
    {
      /* struct_info->_last_error = "Struct_info context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (struct_info->_server_struct_info == NULL)
    {
      /* struct_info->_last_error = "Struct_info server context NULL."; */
      errno = EINVAL;
      return -1;
    }

  if (restart == 1)
    {
      struct_info->_ret_item = struct_info->_items;
      struct_info->_ret_for_server = 0;
      /*DEBUG_MATCHING("restart...\n");*/
    }

  item = struct_info->_ret_item;

  if (!item &&
      struct_info->_ret_for_server == 0)
    {
      /* We are out of client items, continue with the server list. */
      item = struct_info->_ret_item =
	struct_info->_server_struct_info->_items;
      struct_info->_ret_for_server = 1;
      /*
      DEBUG_MATCHING("server... %p %p\n",
		     struct_info->_server_struct_info,
		     struct_info->_ret_item);
      */
    }
  /*
  if (item)
    DEBUG_MATCHING("%s : %x\n", item->_var_name, item->_map_success);
  */
  if (struct_info->_ret_for_server)
    {
      /* For server list, we only report items that
       * are missing destination. */
      while (item &&
	     item->_map_success != EXT_DATA_ITEM_MAP_NO_DEST)
	{
	  /*
	  if (item)
	    DEBUG_MATCHING("%s : %x\n", item->_var_name, item->_map_success);
	  */
	  struct_info->_ret_item = item = item->_next_off_item;
	}
      /*DEBUG_MATCHING("skipped...\n");*/
  }

  if (!item)
    return 0;

  if (var_name)
    *var_name = item->_var_name;

  if (offset)
    {
      if (struct_info->_ret_for_server)
	*offset = (uint32_t) -1;
      else
	*offset = item->_offset;
    }

  if (map_success)
    *map_success = item->_map_success;

  /* Prepare to get next item. */

  struct_info->_ret_item = item->_next_off_item;

  return 1;
}

int
ext_data_struct_info_print_map_success(struct ext_data_structure_info *
				       /* */ struct_info,
				       FILE *stream,
				       uint32_t exclude_success)
{
  int restart = 0;
  const char *var_name;
  uint32_t offset;
  uint32_t map_success;

  fprintf (stream, "%-40s %-8s %-6s\n",
	   "Item", "Offset", "Success");

  fprintf (stream, "%-40s %-8s %-6s\n",
	   "----------------------------------------",
	   "--------", "---------------------------");

  for (restart = 1; ; restart = 0)
    {
      int ret;

      ret =
	ext_data_struct_info_map_success(struct_info,
					 restart,
					 &var_name, &offset, &map_success);

      if (ret == -1)
	return -1;

      if (!ret)
	break;

      if (!(map_success & ~exclude_success))
	continue;

      fprintf (stream, "%-40s %-8d (0x%02x)",
	       var_name, offset, map_success);

#define EXT_DATA_PRINT_MAP_SUCCESS(x,str)  do {		\
	if (map_success & (EXT_DATA_ITEM_MAP_##x)) {	\
	  fprintf (stream, " %s", str);			\
	}						\
      } while (0)

      EXT_DATA_PRINT_MAP_SUCCESS(MATCH,         "match");
      EXT_DATA_PRINT_MAP_SUCCESS(NO_DEST,       "no dest - not mapped");
      EXT_DATA_PRINT_MAP_SUCCESS(NOT_FOUND,     "not found");
      EXT_DATA_PRINT_MAP_SUCCESS(OPT_NOT_FOUND, "optional item not found");
      EXT_DATA_PRINT_MAP_SUCCESS(TYPE_MISMATCH, "type mismatch");
      EXT_DATA_PRINT_MAP_SUCCESS(CTRL_MISMATCH, "ctrl item mismatch");
      EXT_DATA_PRINT_MAP_SUCCESS(ARRAY_FEWER,   "array short");
      EXT_DATA_PRINT_MAP_SUCCESS(ARRAY_MORE,    "array longer");

      fprintf (stream, "\n");
    }

  fprintf (stream, "%-40s %-8s %-6s\n",
	   "----------------------------------------",
	   "--------", "---------------------------");

  return 1;
}

struct ext_data_structure_item* ext_data_structure_item_copy(const struct ext_data_structure_item* from)
{
   const size_t item_size=sizeof(struct ext_data_structure_item);
   // avoiding recursion, who knows how big the stack is. 
   const struct ext_data_structure_item* src=from;
   struct ext_data_structure_item* prev=NULL;
   struct ext_data_structure_item* first=NULL;
   while(src)
   {
      struct ext_data_structure_item* item=(struct ext_data_structure_item*) malloc(item_size);
      memset(item, 0, item_size);
      item->_offset        = src->_offset;
      item->_length        = src->_length;
      item->_var_name      = strdup(src->_var_name);
      item->_var_array_len = src->_var_array_len;
      item->_var_ctrl_name = strdup(src->_var_ctrl_name);
      item->_var_type      = src->_var_type;
      item->_limit_min     = src->_limit_min;
      item->_limit_max     = src->_limit_max;
      item->_flags         = src->_flags;
      item->_next_off_item = NULL;
      if (prev)
	prev->_next_off_item = item;
      if (!first)
	first=item;
      prev=item;
      src=src->_next_off_item;
   }
   return first;
}


int ext_data_struct_info_item(struct ext_data_structure_info *struct_info,
			      size_t offset, size_t size,
			      int type,
			      const char *prename, int preindex,
			      const char *name, const char *ctrl_name,
			      int limit_max, uint32_t flags)
{
  struct ext_data_structure_item **item_ptr;
  struct ext_data_structure_item **item_ptr_before_off;
  struct ext_data_structure_item *item;
  size_t array_items;

  if (!struct_info)
    {
      /* struct_info->_last_error = "Struct_info context NULL."; */
      errno = EFAULT;
      return -1;
    }

  DEBUG_MATCHING("Add item: %s [%s] (0x%zx=%zd:%zd @ 0x%zx=%zd)\n",
		 name, ctrl_name,
		 size/sizeof(uint32_t), size/sizeof(uint32_t), size,
		 offset, offset);

  item = (struct ext_data_structure_item *)
    malloc (sizeof (struct ext_data_structure_item));

  if (!item)
    {
      struct_info->_last_error = "Memory allocation failure (struct item).";
      errno = ENOMEM;
      return -1;
    }

  array_items = size / sizeof (uint32_t);

  item->_offset = (uint32_t) offset;
  item->_length = (uint32_t) size;
  item->_block = NULL;
  item->_var_name = strdup(name);
  item->_var_ctrl_name = strdup(ctrl_name ? ctrl_name : "");
  item->_var_type = type;
  if (limit_max >= 0)
    {
      item->_limit_min = 0;
      item->_limit_max = limit_max;
    }
  else
    {
      item->_limit_min = -1;
      item->_limit_max = -1;
    }
  item->_ctrl_item = NULL;
  item->_flags = flags;

  item->_map_success = (uint32_t) -1;

  /* We better be strict.  Ensure that no previously described item
   * has the same name, or overlaps in the structure.
   *
   * For simplicity, a linear search is used (i.e. we have not sorted
   * the items in any way).  Actually, we can as a sideeffect find out
   * where to place the item, such that they are in structure order.
   */

  item_ptr = &struct_info->_items;
  item_ptr_before_off = item_ptr;

  for ( ; ; )
    {
      struct ext_data_structure_item *check;

      check = *item_ptr;

      if (!check)
	break;

      item_ptr = &check->_next_off_item;

      if (check->_offset < item->_offset)
	item_ptr_before_off = item_ptr;

      if (strcmp(item->_var_name,check->_var_name) == 0)
	{
	  struct_info->_last_error =
	    "Name collision with already declared item.";
	  errno = EINVAL;
	  goto failure_free_return;
	}

      if (check->_offset < item->_offset + item->_length &&
	  check->_offset + check->_length > item->_offset)
	{
	  struct_info->_last_error = "Overlapping with already declared item.";
	  errno = EINVAL;
	  goto failure_free_return;
	}

      /* Find any controlling item. */

      if (strcmp(item->_var_ctrl_name, check->_var_name) == 0)
	{
	  item->_ctrl_item = check;

	  /* TODO: Controlling item must have a limit, and it must not
	   * be larger than our array size.
	   */

	  if (check->_limit_max == (uint32_t) -1)
	    {
	      struct_info->_last_error = "Missing control item limit.";
	      errno = EINVAL;
	      goto failure_free_return;
	    }
	  if (check->_limit_max > array_items)
	    {
	      struct_info->_last_error = "Mismatch with control item limit "
		"(array too small).";
	      errno = EINVAL;
	      goto failure_free_return;
	    }
	}
    }

  if (strcmp(item->_var_ctrl_name,"") != 0 &&
      item->_ctrl_item == NULL)
    {
      struct_info->_last_error = "Controlling item not found.";
      errno = EINVAL;
      goto failure_free_return;
    }

  item->_next_off_item = *item_ptr_before_off;
  *item_ptr_before_off = item;

  return 0;

 failure_free_return:
  free(item);
  /* errno already set */
  return -1;
}

#define MAP_LIST_MARK_LOOP        0x80000000
#define MAP_LIST_SRC_OFFSET_MASK  0x7fffffff

int ext_data_struct_match_items(struct ext_data_client *client,
				struct ext_data_client_struct *clistr,
				const struct ext_data_structure_info *from,
				const struct ext_data_structure_info *to,
				int *all_to_same_from,
				uint32_t *map_success)
{
  struct ext_data_structure_item *item;
  struct ext_data_structure_item *match;
  int items, ctrl_items;
  size_t map_list_items;

  *all_to_same_from = 1;

  /* Since both lists are sorted by offset, it makes sense to do the
   * matching based on the @to list, since we then will do the writing
   * more ordered than if the sorting is by the @from list.
   *
   * Also, this makes it possible to see if all destination items are
   * in the source (at the same locations), in which case we can report
   * a perfect match (and omit the remap copy operation).
   *
   * If a to item is missing in the from list, some other from item
   * may have that location, and thus would fill the location with
   * garbage.  In such cases, the remap copy is needed.
   */

  *map_success = 0;

  /* First clear all temporaries. */

  item = to->_items;

  while (item)
    {
      item->_match_item = NULL;
      item->_child_item = NULL;
      item->_map_success = 0;
      item = item->_next_off_item;
    }

  /* Clear all from temporaries. */

  match = from->_items;

  while (match)
    {
      match->_map_success = 0;
      match = match->_next_off_item;
    }

  /* For each item in the @to list, we want to find possible
   * corresponding items in the @from list.  They need to have the same
   * type, and same controlling item.  Limits of the controlling items
   * may however vary.
   */

  item = to->_items;

  while (item)
    {
      /* We always find the item by name! */

      match = from->_items;

      DEBUG_MATCHING("Matching \'%s\' [%s] ... ",
		     item->_var_name, item->_var_ctrl_name);

      for ( ; ; )
	{
	  if (!match)
	    {
	      if (item->_flags & EXT_DATA_ITEM_FLAGS_OPTIONAL)
		{
		  DEBUG_MATCHING("optional item no match.\n");
		  item->_map_success = EXT_DATA_ITEM_MAP_OPT_NOT_FOUND;
		}
	      else
		{
		  DEBUG_MATCHING("no match.\n");
		  item->_map_success = EXT_DATA_ITEM_MAP_NOT_FOUND;
		}
	      goto no_match;
	    }

	  if (strcmp(item->_var_name, match->_var_name) == 0)
	    break;

	  match = match->_next_off_item;
 	}

      /* The types must match. */

      if ((item->_var_type & EXT_DATA_ITEM_TYPE_MASK) !=
	  (match->_var_type & EXT_DATA_ITEM_TYPE_MASK))
	{
	  DEBUG_MATCHING("type mismatch (%d -> %d).\n",
			 match->_var_type, item->_var_type);
	  item->_map_success =
	    match->_map_success = EXT_DATA_ITEM_MAP_TYPE_MISMATCH;
	  goto no_match;
	}

      /* If it has a controlling item, it must match. */

      if (strcmp(item->_var_ctrl_name, match->_var_ctrl_name) != 0)
	{
	  DEBUG_MATCHING("ctrl mismatch ([%s] -> ).\n",
			 match->_var_ctrl_name);
	  item->_map_success =
	    match->_map_success = EXT_DATA_ITEM_MAP_CTRL_MISMATCH;
	  goto no_match;
	}

      /* So it is a good match. */

      item->_match_item = match;

      if (item->_length == match->_length)
	{
	  item->_map_success =
	    match->_map_success = EXT_DATA_ITEM_MAP_MATCH;
	}
      else if (item->_length < match->_length)
	{
	  item->_map_success  = EXT_DATA_ITEM_MAP_ARRAY_FEWER;
	  match->_map_success = EXT_DATA_ITEM_MAP_ARRAY_MORE;
	}
      else /* (item->_length > match->_length) */
	{
	  item->_map_success  = EXT_DATA_ITEM_MAP_ARRAY_MORE;
	  match->_map_success = EXT_DATA_ITEM_MAP_ARRAY_FEWER;
	}

      /* If we have a controlling item, it shall be added to that list.
       */

      if (item->_ctrl_item)
	{
	  struct ext_data_structure_item **add_ptr =
	    &item->_ctrl_item->_child_item;

	  while (*add_ptr)
	    add_ptr = &(*add_ptr)->_child_item;

	  *add_ptr = item;
	}

      if (item->_offset != match->_offset ||
	  item->_length != match->_length)
	*all_to_same_from = 0;

      DEBUG_MATCHING("ok!\n");

      goto next_item;

    no_match:
      *all_to_same_from = 0;
    next_item:
      *map_success |= item->_map_success;
      item = item->_next_off_item;
    }

  DEBUG_MATCHING("Done matching items.\n");
  
  /* Mark all source items that were not matched. */
  
  match = from->_items;

  while (match)
    {
      if (!match->_map_success)
	{
	  DEBUG_MATCHING("No match for \'%s\' [%s], missing dest.\n",
			 match->_var_name, match->_var_ctrl_name);
	  *map_success |=
	    match->_map_success = EXT_DATA_ITEM_MAP_NO_DEST;
	}
      match = match->_next_off_item;
    }

  /* How much will it contribute to the remap list size?
   * Each array item contributes two offsets per used item.
   * A plain item contributes two offsets.
   * If it is a controlling item, it contributes another two
   * loop info items.
   */

  items = 0;
  ctrl_items = 0;

  item = to->_items;

  while (item)
    {
      if (item->_ctrl_item)
	; /* We are handled inside the ctrl item. */
      else if (item->_match_item)
	{
	  items++;

	  if (item->_child_item)
	    {
	      struct ext_data_structure_item *child;
	      int child_items = 0;

	      ctrl_items++;

	      child = item->_child_item;

	      while (child)
		{
		  child_items++;
		  child = child->_child_item;
		}

	      items += child_items;
	    }
	}

      item = item->_next_off_item;
    }

  map_list_items = 2 * items + 2 * ctrl_items;

  DEBUG_MATCHING("raw map list items: %zd (%d, %d)\n",
		 map_list_items, items, ctrl_items);

  /* Allocate the map list. */

  clistr->_map_list =
    (uint32_t *) malloc (map_list_items * sizeof(uint32_t));

  if (!clistr->_map_list)
    {
      client->_last_error = "Memory allocation failure (map list).";
      errno = ENOMEM;
      return -1;
    }

  clistr->_map_list_end = clistr->_map_list + map_list_items;

  /* And fill the map list. */

  uint32_t *o    = clistr->_map_list;

  item = to->_items;

  while (item)
    {
      if (item->_ctrl_item)
	; /* We are handled inside the ctrl item. */
      else if (item->_match_item)
	{
	  uint32_t *o_mark = o;

	  *(o++) = item->_match_item->_offset; /* src  */
	  *(o++) = item->_offset;              /* dest */

	  if (item->_child_item)
	    {
	      struct ext_data_structure_item *child;
	      int child_items = 0;
	      uint32_t *o_loop_size;
	      uint32_t max_loops;

	      max_loops = item->_match_item->_limit_max;
	      if (item->_limit_max < max_loops)
		max_loops = item->_limit_max;

	      *o_mark |= MAP_LIST_MARK_LOOP; /* mark | [dest] */
	      *(o++) = max_loops;              /* loops */
	      o_loop_size = o++;               /* items */

	      child = item->_child_item;

	      while (child)
		{
		  *(o++) = child->_match_item->_offset; /* src  */
		  *(o++) = child->_offset;              /* dest */

		  child_items++;
		  child = child->_child_item;
		}

	      *o_loop_size = child_items;      /* items */
	    }
	}

      item = item->_next_off_item;
    }

  assert (o == clistr->_map_list_end);

  o = clistr->_map_list;

  for (o = clistr->_map_list; o < clistr->_map_list_end; o++)
    DEBUG_MATCHING("0x%08x\n", *o);

  return 0;
}



/* This function is for internal use.  It copies data from one
 * structure layout to another.  It is similar to the offset lists
 * that come with the stream, but the lists are set up internally.
 */

static void
ext_data_struct_map_items(const struct ext_data_client_struct *clistr,
			  char *dest, char *src)
{
  uint32_t *o    = clistr->_map_list;
  uint32_t *oend = clistr->_map_list_end;

  while (o < oend)
    {
      uint32_t offset_src_mark = *(o++);
      uint32_t offset_dest     = *(o++);
      uint32_t offset_src = offset_src_mark & MAP_LIST_SRC_OFFSET_MASK;
      uint32_t loop = offset_src_mark & MAP_LIST_MARK_LOOP;
      uint32_t value;
      /*
      printf ("0x%08x 0x%08x : %5d %5d %s\n",
              offset_src_mark, offset_dest,
	      offset_src, offset_dest, mark ? "l" : "");
      fflush(stdout);
      */
      value = *((uint32_t *) (src + offset_src));

      if (loop)
	{
	  uint32_t max_loops = *(o++);
	  uint32_t loop_size = *(o++);

	  if (value > max_loops)
	    value = max_loops;

	  *((uint32_t *) (dest + offset_dest)) = value;

	  uint32_t *onext = o + 2 * loop_size;

	  if (value)
	    {
	      uint32_t i, j;

	      for (i = loop_size; i; i--)
		{
		  offset_src_mark = *(o++);
		  offset_dest     = *(o++);
		  offset_src = offset_src_mark & 0x3fffffff;

		  for (j = 0; j < value * sizeof (uint32_t);
		       j += sizeof (uint32_t))
		    {
		      uint32_t v = *((uint32_t *) (src + offset_src + j));
		      *((uint32_t *) (dest + offset_dest + j)) = v;
		    }
		}
	    }

	  o = onext;
	}
      else
	{
	  *((uint32_t *) (dest + offset_dest)) = value;
	}
    }
}

/* This function is intended for internal use.  It ensures an entire
 * message is ready in the receive buffer.  It does not consume the
 * message.
 */

struct external_writer_buf_header *
ext_data_peek_message(struct ext_data_client *client)
{
  struct external_writer_buf_header *header;

  for ( ; ; )
    {
      size_t avail = client->_buf_filled - client->_buf_used;

      if (avail >= sizeof(struct external_writer_buf_header))
	{
	  header = (struct external_writer_buf_header *)
	    (client->_buf + client->_buf_used);

	  if (avail >= ntohl(header->_length))
	    break; /* An entire message is available. */
	}

      if (client->_buf_filled == client->_buf_alloc)
	{
	  /* Buffer filled to the end. */

	  if (client->_buf_used)
	    {
	      /* There is empty space at the beginning, first move
	       * the data there.
	       */

	      memmove(client->_buf,
		      client->_buf + client->_buf_used,
		      client->_buf_filled - client->_buf_used);

	      client->_buf_filled -= client->_buf_used;
	      client->_buf_used = 0;
	    }
	  else
	    {
	      /* Buffer completely full.  And no entire message.  This
	       * cannot happen unless the data is corrupt, as we are
	       * always told about the maximum size possible.
	       */

	      client->_last_error =
		"Buffer completely full while receiving message.";
	      errno = EBADMSG;
	      return NULL;
	    }
	}

      size_t remain = client->_buf_alloc - client->_buf_filled;

      ssize_t n =
	read(client->_fd,client->_buf+client->_buf_filled,remain);

      if (n == 0)
	{
	  /* Out of data. */
	  if (client->_buf_used == client->_buf_filled)
	    {
	      client->_last_error = "Out of data.";
	      errno = EBADMSG;
	      return NULL;
	    }

	  /* Out of data, but partial message received.
	   */

	  client->_last_error = "Out of data while receiving message.";
	  errno = EBADMSG;
	  return NULL;
	}

      if (n == -1)
	{
	  if (errno == EINTR)
	    continue;
	  if (errno == EAGAIN)
	    {
	      client->_last_error = "No more data yet, "
		"for non-blocking client.";
	      /* errno already set */
	      return NULL;
	    }
	  client->_last_error = "Failure reading buffer data.";
	  /* Failure. */
	  return NULL;
	}

      client->_buf_filled += (size_t) n;
    }

  /* Unaligned messages are no good. */

  if ((ntohl(header->_request) & EXTERNAL_WRITER_REQUEST_HI_MASK) !=
      EXTERNAL_WRITER_REQUEST_HI_MAGIC)
    {
      client->_last_error = "Message request hi magic wrong.";
      errno = EPROTO;
      return NULL;
    }

  if (ntohl(header->_length) & 3)
    {
      client->_last_error = "Message length unaligned.";
      errno = EPROTO;
      return NULL;
    }

  return header;
}

static void ext_data_clistr_free(struct ext_data_client_struct *clistr)
{
  free((char *) clistr->_id);

  free(clistr->_orig_array);
  free(clistr->_orig_pack_list);
  free(clistr->_dest_pack_list);
  free(clistr->_dest_reverse_pack);
  free(clistr->_map_list);

  ext_data_struct_info_free(clistr->_struct_info_msg);
}

static void ext_data_free(struct ext_data_client *client)
{
  int i;

  free(client->_buf);

  for (i = 0; i < client->_num_structures; i++)
    ext_data_clistr_free(client->_structures+i);

  free(client->_structures);
  free(client->_raw_swapped);

  free(client); /* Note! we also free the structure itself. */
}

void ext_data_clear_client_struct(struct ext_data_client_struct *clistr)
{
  clistr->_id = NULL;

  clistr->_max_raw_words = 0;

  clistr->_orig_xor_sum_msg = 0;
  clistr->_orig_struct_size = 0;
  clistr->_orig_array = NULL;
  clistr->_orig_max_pack_items = 0;
  clistr->_orig_static_pack_items = 0;

  clistr->_orig_pack_list = NULL;
  clistr->_orig_pack_list_end = NULL;

  clistr->_dest_struct_size = 0;
  clistr->_dest_max_pack_items = 0;
  clistr->_dest_static_pack_items = 0;

  clistr->_dest_pack_list = NULL;
  clistr->_dest_pack_list_end = NULL;
  clistr->_dest_reverse_pack = NULL;

  clistr->_map_list = NULL;
  clistr->_map_list_end = NULL;

  clistr->_struct_info_msg = NULL;
}

struct ext_data_client *ext_data_create_client(size_t buf_alloc)
{
  struct ext_data_client *client;

  client = (struct ext_data_client *) malloc (sizeof (struct ext_data_client));

  if (!client)
    {
      errno = ENOMEM;
      return NULL;
    }

  client->_structures = NULL;
  client->_num_structures = 0;

  client->_sort_u32_words = (uint32_t) -1;

  client->_fd_close = -1;

  client->_buf = NULL;
  client->_buf_alloc = 0;
  client->_buf_used = 0;
  client->_buf_filled = 0;

  client->_raw_ptr = NULL;
  client->_raw_words = 0;
  client->_raw_swapped = NULL;

  client->_last_error = NULL;

  client->_state = EXT_DATA_STATE_INIT;

  client->_fetched_event = 0;

  if (buf_alloc)
    {
      /* Get us a buffer for reading. */

      client->_buf = (char *) malloc (EXTERNAL_WRITER_MIN_SHARED_SIZE);

      if (!client->_buf)
	{
	  client->_last_error = "Memory allocation failure (buf).";
	  errno = ENOMEM;
	  return NULL;
	}

      client->_buf_alloc = EXTERNAL_WRITER_MIN_SHARED_SIZE;
    }

  return client;
}

static struct ext_data_client_struct *
ext_data_alloc_client_struct(struct ext_data_client *client)
{
  struct ext_data_client_struct *clistr;

  client->_structures =
    (struct ext_data_client_struct *)
    realloc (client->_structures,
	     (client->_num_structures + 1) *
	     sizeof (struct ext_data_client_struct));

  if (!client->_structures)
    {
      client->_last_error =
	"Memory allocation failure (client structure).";
      errno = ENOMEM;
      return NULL;
    }

  clistr = &client->_structures[client->_num_structures];

  client->_num_structures++;

  ext_data_clear_client_struct(clistr);

  return clistr;
}

static int ext_data_send_magic(struct ext_data_client *client)
{
  size_t sent;
  uint32_t magic = htonl(~EXTERNAL_WRITER_MAGIC);

  /* Send a 32-bit magic, to distinguish us from a possible http
   * client.
   */

  sent = 0;

  while (sent < sizeof(magic))
    {
      fd_set writefds;
      int nfd;
      struct timeval timeout;
      int ret;
      size_t left;
      ssize_t n;

      FD_ZERO(&writefds);
      FD_SET(client->_fd,&writefds);
      nfd = client->_fd;

      timeout.tv_sec = 2;
      timeout.tv_usec = 0;

      ret = select(nfd+1,NULL,&writefds,NULL,&timeout);

      if (ret == -1)
	{
	  if (errno == EINTR)
	    continue;
	  return 0;
	}

      if (ret == 0) /* Can only happen on timeout. */
	{
	  errno = ETIMEDOUT;
	  return 0;
	}

      left = sizeof(magic) - sent;

      n = write(client->_fd,((char*) &magic)+sent,left);

      if (n == -1)
	{
	  if (errno == EINTR)
	    continue;
	  return 0;
	}
      if (n == 0)
	{
	  errno = EPROTO;
	  return 0;
	}

      sent += (size_t) n;
    }

  return 1;
}


struct ext_data_client *ext_data_connect(const char *server)
{
  struct ext_data_client *client;

  if (!(client = ext_data_create_client(EXTERNAL_WRITER_MIN_SHARED_SIZE)))
    return NULL; // errno already set

  if (strcmp(server,"-") == 0)
    {
      /* Read data from stdin. */

      client->_fd = STDIN_FILENO;
      client->_state = EXT_DATA_STATE_OPEN;

      return client;
    }
  else
    {
      int rc;
      struct sockaddr_in serv_addr;
      struct hostent *h;
      char *hostname, *colon;
      unsigned short port = (unsigned short) EXTERNAL_WRITER_DEFAULT_PORT;
      struct external_writer_portmap_msg portmap_msg;
      size_t got;

      /* If there is a colon in the hostname, interpret what follows
       * as a port number, overriding the default port.
       */
      hostname = strdup(server);
      colon = strchr(hostname,':');

      if (colon)
	{
	  port = (unsigned short) atoi(colon+1);
	  *colon = 0; // cut the hostname
	}

      /* Get server IP address. */
      h = gethostbyname(hostname);
      free(hostname);

      if(h == NULL)
	{
	  /* EHOSTDOWN is not really correct, but the best I could find. */
	  errno = EHOSTDOWN;
	  goto errno_return_NULL;
	}

      /*
	INFO("Server '%s' known... (IP : %s).", h->h_name,
	inet_ntoa(*(struct in_addr *)h->h_addr_list[0]));
      */

      /* Create the socket. */
      client->_fd_close = client->_fd =
	socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
      if (client->_fd == -1)
	goto errno_return_NULL;

      /* Bind to the port. */
      serv_addr.sin_family = (sa_family_t) h->h_addrtype;
      memcpy((char *) &serv_addr.sin_addr.s_addr,
	     h->h_addr_list[0], (size_t) h->h_length);
      serv_addr.sin_port = htons(port);

      rc = connect(client->_fd,
		   (struct sockaddr *) &serv_addr, sizeof(serv_addr));
      if (rc == -1)
	goto errno_return_NULL;

      /* Send a 32-bit magic, to distinguish us from a possible http
       * client.
       */

      if (!ext_data_send_magic(client))
	goto errno_return_NULL;

      /* We've connected to the port server, get the address.  We
       * employ a time-out for the portmap message, to not get stuck
       * here.
       */

      got = 0;

      while (got < sizeof(portmap_msg))
	{
	  fd_set readfds;
	  int nfd;
	  struct timeval timeout;
	  int ret;
	  size_t left;
	  ssize_t n;

	  FD_ZERO(&readfds);
	  FD_SET(client->_fd,&readfds);
	  nfd = client->_fd;

	  timeout.tv_sec = 2;
	  timeout.tv_usec = 0;

	  ret = select(nfd+1,&readfds,NULL,NULL,&timeout);

	  if (ret == -1)
	    {
	      if (errno == EINTR)
		continue;
	      goto errno_return_NULL;
	    }

	  if (ret == 0) /* Can only happen on timeout. */
	    {
	      errno = ETIMEDOUT;
	      goto errno_return_NULL;
	    }

	  left = sizeof(portmap_msg) - got;

	  n = read(client->_fd,((char*) &portmap_msg)+got,left);

	  if (n == -1)
	    {
	      if (errno == EINTR)
		continue;
	      goto errno_return_NULL;
	    }
	  if (n == 0)
	    {
	      errno = EPROTO;
	      goto errno_return_NULL;
	    }

	  got += (size_t) n;
	}

      for ( ; ; )
	{
	  if (close(client->_fd) == 0)
	    break;
	  if (errno == EINTR)
	    continue;
	  goto errno_return_NULL;
	}

      client->_fd_close = client->_fd = -1;

      if (portmap_msg._magic != htonl(~EXTERNAL_WRITER_MAGIC))
	{
	  errno = EPROTO;
	  goto errno_return_NULL;
	}

      // Now, connect to the data port!

      /* Create the socket. */
      client->_fd_close = client->_fd =
	socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
      if (client->_fd == -1)
	goto errno_return_NULL;

      /* Connect to the port. */
      serv_addr.sin_family = (sa_family_t) h->h_addrtype;
      memcpy((char *) &serv_addr.sin_addr.s_addr,
	     h->h_addr_list[0], (size_t) h->h_length);
      serv_addr.sin_port = portmap_msg._port;

      rc = connect(client->_fd,
		   (struct sockaddr *) &serv_addr, sizeof(serv_addr));
      if (rc == -1)
	goto errno_return_NULL;

      /* We have successfully connected. */

      /* Send a 32-bit magic, to distinguish us from a possible http
       * client.
       */

      if (!ext_data_send_magic(client))
	goto errno_return_NULL;

      client->_state = EXT_DATA_STATE_OPEN;

      return client;
    }

  /* We never come this way. */

 errno_return_NULL:
  /* Free the allocated client buffer.
   * The data buffer also, if allocated.
   */
  {
    int errsv = errno;
    /* We do not care about close errors. */
    if (client->_fd_close != -1)
      close(client->_fd_close);
    /* Cannot fail, could possibly change errno. */
    ext_data_free(client);
    errno = errsv;
  }
  return NULL;
}

struct ext_data_client *ext_data_from_fd(int fd)
{
  struct ext_data_client *client;

  if (!(client = ext_data_create_client(EXTERNAL_WRITER_MIN_SHARED_SIZE)))
    return NULL; // errno already set

  client->_fd = fd;
  client->_state = EXT_DATA_STATE_OPEN;

  return client;
}

struct ext_data_client *ext_data_open_out()
{
  struct ext_data_client *client;

  if (!(client = ext_data_create_client(0)))
    return NULL; // errno already set

  client->_fd = STDOUT_FILENO;
  client->_state = EXT_DATA_STATE_OPEN_OUT;

  return client;
}

int ext_data_nonblocking_fd(struct ext_data_client *client)
{
  if (!client)
    {
      /* client->_last_error = "Client context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (client->_state < EXT_DATA_STATE_SETUP_READ)
    {
      client->_last_error = "Client context has not had setup.";
      errno = EFAULT;
      return -1;
    }

  if (fcntl(client->_fd,F_SETFL,
	    fcntl(client->_fd,F_GETFL) | O_NONBLOCK) == -1)
    {
      client->_last_error = "Failure making file descriptor non-blocking";
      /* errno already set */
      return -1;
    }

  return client->_fd;
}

const char *ext_data_extr_str(uint32_t **p, uint32_t *length_left)
{
  uint32_t str_len, str_align_len;
  const char *str;
  if (*length_left < sizeof(uint32_t))
    return NULL;
  str_len = ntohl((*p)[0]);
  (*p)++;
  *length_left -= sizeof(uint32_t);
  str_align_len = ((str_len+1) + 3) & ~3;
  if (*length_left < str_align_len)
    return NULL;
  str = (const char *) (*p);
  if (str[str_len] != 0)
    return NULL;
  *p = (uint32_t *) (((char *) (*p)) + str_align_len);
  *length_left -= str_align_len;
  return str;
}

/* Handle messages that come during setup phase. */
static int ext_data_setup_messages(struct ext_data_client *client)
{
  struct ext_data_client_struct *clistr = NULL;

  for ( ; ; )
    {
      struct external_writer_buf_header *header;

      header = ext_data_peek_message(client);

      if (header == NULL)
	return -1;

      switch (ntohl(header->_request) & EXTERNAL_WRITER_REQUEST_LO_MASK)
	{
	case EXTERNAL_WRITER_BUF_OPEN_FILE:
	  {
	    uint32_t magic;
	    uint32_t sort_u32_words;
	    uint32_t *p = (uint32_t *) (header+1);

	    if (ntohl(header->_length) <
		sizeof(struct external_writer_buf_header) + 2*sizeof(uint32_t))
	      {
		client->_last_error =
		  "Bad open message size during setup.";
		errno = EPROTO;
		return -1;
	      }

	    magic = ntohl(p[0]);

	    if (magic != EXTERNAL_WRITER_MAGIC)
	      {
		client->_last_error =
		  "Bad open message magic during setup.";
		errno = EPROTO;
		return -1;
	      }

	    sort_u32_words = ntohl(p[1]);
	    client->_sort_u32_words = sort_u32_words;

	    break;
	  }

	case EXTERNAL_WRITER_BUF_ALLOC_ARRAY:
	  {
	    uint32_t *p = (uint32_t *) (header+1);

	    if (ntohl(header->_length) <
		sizeof(struct external_writer_buf_header) + sizeof(uint32_t))
	      {
		client->_last_error =
		  "Bad alloc message size during setup.";
		errno = EPROTO;
		return -1;
	      }

	    if (!clistr)
	      {
		client->_last_error =
		  "Cannot handle alloc message without structure.";
		errno = EPROTO;
		return -1;
	      }

	    clistr->_orig_struct_size = ntohl(p[0]);

	    break;
	  }

	case EXTERNAL_WRITER_BUF_ARRAY_OFFSETS:
	  {
	    uint32_t *p = (uint32_t *) (header+1);
	    uint32_t length_left = ntohl(header->_length) -
	      sizeof(struct external_writer_buf_header);
	    uint32_t pack_list_items;

	    if (length_left < sizeof(uint32_t))
	      {
		client->_last_error =
		  "Bad array offsets message size during setup.";
		errno = EPROTO;
		return -1;
	      }

	    if (!clistr)
	      {
		client->_last_error =
		  "Cannot handle offset message without structure.";
		errno = EPROTO;
		return -1;
	      }

	    clistr->_orig_xor_sum_msg = ntohl(p[0]);

	    p++;
	    length_left -= sizeof(uint32_t);

	    /* And then follows the actual offset array... */
	    /* Make our own copy of the pack list. */

	    pack_list_items = length_left / sizeof(uint32_t);

	    clistr->_orig_pack_list =
	      (uint32_t *) malloc (pack_list_items * sizeof(uint32_t));
	    
	    if (!clistr->_orig_pack_list)
	      {
		client->_last_error =
		  "Memory allocation failure (orig pack list).";
		errno = ENOMEM;
		return -1;
	      }

	    clistr->_orig_pack_list_end =
	      clistr->_orig_pack_list + pack_list_items;

	    /* TODO: verify that offsets match the list we have! */
	    /* Note: if it is from a writer, there will be no offsets
	     * (at least yet), only the xor.
	     */

      {
	uint32_t *o    = p;
	uint32_t *oend = o + pack_list_items;

	uint32_t *d = clistr->_orig_pack_list;

	clistr->_orig_max_pack_items = 0;
	clistr->_orig_static_pack_items = 0;

	/* fprintf (stderr, "PACK_LIST: %d\n", pack_list_items); */

	while (o < oend)
	  {
	    uint32_t mark   = *(d++) = ntohl(*(o++));
	    uint32_t offset = *(d++) = ntohl(*(o++));
	    uint32_t loop = mark & EXTERNAL_WRITER_MARK_LOOP;

	    (void) offset;

	    /* fprintf (stderr, "PL, OM: %08x\n", mark); */

	    clistr->_orig_static_pack_items++;

	    if (loop)
	      {
		uint32_t max_loops = *(d++) = ntohl(*(o++));
		uint32_t loop_size = *(d++) = ntohl(*(o++));

		uint32_t items = max_loops * loop_size;
		uint32_t i;

		/*fprintf (stderr, "PL, LOOP: %08x %08x\n",
		  max_loops, loop_size);*/

		if (oend - o < (ssize_t) (2 * items))
		  {
		    client->_last_error =
		      "Pack-list malformed, too many loop items.";
		    errno = EPROTO;
		    return -1;
		  }

		clistr->_orig_max_pack_items += items;

		for (i = items; i; i--)
		  {
		    mark   = *(d++) = ntohl(*(o++));
		    offset = *(d++) = ntohl(*(o++));

		    /* fprintf (stderr, "PL, Li: %08x %08x\n", mark, offset); */
		  }
	      }
	  }
	clistr->_orig_max_pack_items += clistr->_orig_static_pack_items;

	if (d != clistr->_orig_pack_list_end)
	  {
	    client->_last_error =
	      "Pack-list malformed, copy failure.";
	    errno = EPROTO;
	    return -1;
	  }
	
	/*
  	fprintf (stderr,
		 "PACK_LIST_END: %d %d\n",
		 clistr->_orig_static_pack_items,
		 clistr->_orig_max_pack_items);
	*/
    }

            /* TODO: what is this?: */
	    clistr = NULL;

	    break;
	  }
	  
	case EXTERNAL_WRITER_BUF_BOOK_NTUPLE:
	  {
	    uint32_t *p = (uint32_t *) (header+1);

	    uint32_t struct_index;
	    uint32_t ntuple_index;
	    uint32_t hid;
	    uint32_t max_raw_words;

	    const char *id = NULL;
	    const char *title = NULL;
	    const char *index_major = NULL;
	    const char *index_minor = NULL;

	    uint32_t length_left = ntohl(header->_length) -
	      sizeof(struct external_writer_buf_header);

	    if (length_left < 3 * sizeof(uint32_t))
	      {
		client->_last_error =
		  "Bad ntuple booking message size during setup.";
		errno = EPROTO;
		return -1;
	      }

	    if (clistr)
	      {
		client->_last_error =
		  "Cannot do ntuple booking with active structure.";
		errno = EPROTO;
		return -1;
	      }

	    struct_index = ntohl(p[0]);
	    ntuple_index = ntohl(p[1]);
	    hid = ntohl(p[2]);
	    (void) hid;

	    p += 3;
	    length_left -= 3 * sizeof(uint32_t);

	    if ((int) struct_index != client->_num_structures)
	      {
		client->_last_error =
		  "Ntuple structure index not in order during setup.";
		errno = EPROTO;
		return -1;
	      }

	    /* fprintf (stderr, "STRUCT_INDEX: %d\n", struct_index); */
	    clistr = ext_data_alloc_client_struct(client);

	    if (!clistr)
	      return -1;

	    if (ntuple_index == 0)
	      {
		if (length_left < 1 * sizeof(uint32_t))
		  {
		    client->_last_error =
		      "Bad ntuple booking message size (ii) during setup.";
		    errno = EPROTO;
		    return -1;
		  }

		max_raw_words = ntohl(p[0]);
		clistr->_max_raw_words = max_raw_words;

		p += 1;
		length_left -= 1 * sizeof(uint32_t);

		if (ntohl(0x01020304) != 0x01020304)
		  {
		    /* We need a temporary array. */
		    client->_raw_swapped = (uint32_t *)
		      malloc (clistr->_max_raw_words * sizeof (uint32_t));
		    if (!client->_raw_swapped)
		      {
			client->_last_error =
			  "Memory allocation failure (raw swapped).";
			errno = ENOMEM;
			return -1;
		      }
		  }
	      }

	    if ((id = ext_data_extr_str(&p, &length_left)) == NULL ||
		(title = ext_data_extr_str(&p, &length_left)) == NULL ||
		(index_major = ext_data_extr_str(&p, &length_left)) == NULL ||
		(index_minor = ext_data_extr_str(&p, &length_left)) == NULL)
	      goto book_ntuple_protocol_error;

	    (void) id;
	    (void) title;

	    if (ntuple_index == 0)
	      {
		clistr->_id = strdup(id);
	      }

	    if (length_left)
	      goto book_ntuple_protocol_error;

	    /* Retain the information to be able to handle multiple
	     * setups, in arbitrary order.
	     */
	    clistr->_struct_info_msg = ext_data_struct_info_alloc();

	    if (clistr->_struct_info_msg == NULL)
	      {
		client->_last_error =
		  "Memory allocation failure (struct info).";
		return -1; // errno already set
	      }

	    break;
	  }
	  {
	  book_ntuple_protocol_error:
	    client->_last_error =
	      "Bad ntuple booking message content during setup.";
	    errno = EPROTO;
	    return -1;
	  }

	case EXTERNAL_WRITER_BUF_CREATE_BRANCH:
	  {
	    uint32_t offset;
	    uint32_t length;
	    uint32_t var_array_len;
	    uint32_t var_type;
	    uint32_t limit_min;
	    uint32_t limit_max;
	    uint32_t *p = (uint32_t *) (header+1);
	    const char *block = NULL;
	    const char *var_name = NULL;
	    const char *var_ctrl_name = NULL;

	    uint32_t length_left = ntohl(header->_length) -
	      sizeof(struct external_writer_buf_header);

	    if (length_left < 6 * sizeof(uint32_t))
	      {
		client->_last_error =
		  "Bad create branch message size during setup.";
		errno = EPROTO;
		return -1;
	      }

	    if (!clistr)
	      {
		client->_last_error =
		  "Cannot handle create branch message without structure.";
		errno = EPROTO;
		return -1;
	      }

	    offset    = ntohl(p[0]);
	    length    = ntohl(p[1]);
	    var_array_len = ntohl(p[2]);
	    var_type  = ntohl(p[3]);
	    limit_min = ntohl(p[4]);
	    limit_max = ntohl(p[5]);

	    p += 6;
	    length_left -= 6 * sizeof(uint32_t);

	    if ((block = ext_data_extr_str(&p, &length_left)) == NULL ||
		(var_name = ext_data_extr_str(&p, &length_left)) == NULL ||
		(var_ctrl_name = ext_data_extr_str(&p, &length_left)) == NULL)
	      goto create_branch_protocol_error;

	    if (length_left)
	      goto create_branch_protocol_error;

	    (void) var_array_len;
	    (void) var_type;
            (void) limit_min;

	    //printf ("Q: %s %s %s\n",block, var_name, var_ctrl_name);

	    ext_data_struct_info_item(clistr->_struct_info_msg,
				      offset, length,
				      var_type,
				      "", -1,
				      var_name, var_ctrl_name,
				      limit_max, 0);

	    break;
	  }
	  {
	  create_branch_protocol_error:
	    client->_last_error =
	      "Bad create branch message content during setup.";
	    errno = EPROTO;
	    return -1;
	  }

	case EXTERNAL_WRITER_BUF_NAMED_STRING:
	  /* fprintf (stderr, "named string ignored in setup\n"); */
	  break;

	case EXTERNAL_WRITER_BUF_NTUPLE_FILL:
	case EXTERNAL_WRITER_BUF_DONE:
	case EXTERNAL_WRITER_BUF_ABORT:
	  /* Not allowed until we're set up. */
	default:
	  /* Unexpected message, not allowed. */
	  client->_last_error = "Unexpected message during setup.";
	  errno = EPROTO;
	  return -1;

	case EXTERNAL_WRITER_BUF_RESIZE:
	  {
	    uint32_t newsize, magic;
	    uint32_t *p = (uint32_t *) (header+1);

	    /* Resize our receive buffer, to be able to receive the
	     * maximum size messages that may arrive.
	     */

	    if (ntohl(header->_length) <
		sizeof(struct external_writer_buf_header) + 2*sizeof(uint32_t))
	      {
		client->_last_error =
		  "Bad resize message during setup.";
		errno = EPROTO;
		return -1;
	      }

	    newsize = ntohl(p[0]);
	    magic   = ntohl(p[1]);

	    if (magic != EXTERNAL_WRITER_MAGIC)
	      {
		client->_last_error =
		  "Bad resize message magic during setup.";
		errno = EPROTO;
		return -1;
	      }

	    char *newbuf = (char *) realloc (client->_buf,newsize);

	    if (!newbuf)
	      {
		client->_last_error =
		  "Memory allocation failure (buf resize).";
		errno = ENOMEM;
		return -1;
	      }

	    client->_buf       = newbuf;
	    client->_buf_alloc = newsize;

	    /* Since we did a reallocation. */

	    header = (struct external_writer_buf_header *)
	      (client->_buf + client->_buf_used);
	    break;
	  }

	case EXTERNAL_WRITER_BUF_SETUP_DONE:
	case EXTERNAL_WRITER_BUF_SETUP_DONE_WR:
	  {
	    uint32_t *p = (uint32_t *) (header+1);

	    if (ntohl(header->_length) <
		sizeof(struct external_writer_buf_header))
	      {
		client->_last_error =
		  "Bad setup done message size during setup.";
		errno = EPROTO;
		return -1;
	      }

	    if (clistr)
	      {
		client->_last_error =
		  "Cannot handle setup done with active structure.";
		errno = EPROTO;
		return -1;
	      }

	    (void) p;

	    /* Consume the message. */
	    client->_buf_used += ntohl(header->_length);

	    /* Goto while we develop the movement of the message handling. */
	    goto messages_done;
	  }
	}

      /* Consume the accepted/ignored message. */

      client->_buf_used += ntohl(header->_length);
    }

 messages_done:

  return 0;
}

int ext_data_setup(struct ext_data_client *client,
		   const void *struct_layout_info,size_t size_info,
		   struct ext_data_structure_info *struct_info,
		   uint32_t *struct_map_success,
		   size_t size_buf,
		   const char *name_id, int *struct_id)
{
  struct ext_data_client_struct *clistr = NULL;
  const struct ext_data_structure_layout *slo =
    (const struct ext_data_structure_layout *) struct_layout_info;
  const struct ext_data_structure_layout_item *slo_items;
  const uint32_t *slo_pack_list;
  uint32_t i;

  if (struct_map_success)
    *struct_map_success = EXT_DATA_ITEM_MAP_NOT_DONE;

  if (!client)
    {
      /* client->_last_error = "Client context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (client->_state < EXT_DATA_STATE_OPEN)
    {
      /* This error shall not happen, since to create a structure, it
       * will be connected or opened.
       */
      client->_last_error =
	"Client context has not been opened (internal error).";
      errno = EFAULT;
      return -1;
    }

  if (!slo && !struct_info)
    {
      client->_last_error = "No structure layout or items info.";
      errno = EFAULT;
      return -1;
    }

  if (slo && struct_info)
    {
      /* Hmm, if we would be writing, we would need to handle this
       * in case the user would like to take the data from an
       * alternative structure.  But we do not handle such picking
       * yet...
       */
      client->_last_error = "Both structure layout and items info.";
      errno = EFAULT;
      return -1;
    }

  /* Before we start to investigate the information given, see if we
   * can find the requested structure at all.  First thing to do is to
   * parse the data from the server.
   */

  if (client->_state == EXT_DATA_STATE_OPEN)
    {
      /* To allow handling of multiple structures, the messages are only
       * read once, and then verified.
       */

      if (ext_data_setup_messages(client) == -1)
	return -1;

      if (client->_num_structures < 1)
	{
	  client->_last_error = "No structure (ntuple) from server.";
	  errno = EPROTO;
	  return -1;
	}

      client->_state = EXT_DATA_STATE_PARSED_HEADERS;
    }

  if (client->_state == EXT_DATA_STATE_OPEN_OUT)
    {
      assert(client->_num_structures == 0);

      clistr = ext_data_alloc_client_struct(client);

      if (!clistr)
	return -1;
    }

  if (client->_state != EXT_DATA_STATE_OPEN_OUT)
    {
      /* TODO: Match the name requested!!! */

      if (strcmp(name_id, "") == 0)
	{
	  clistr = &client->_structures[0];
	  if (struct_id)
	    *struct_id = 0;
	}
      else
	{
	  int i;

	  for (i = 0; i < client->_num_structures; i++)
	    {
	      struct ext_data_client_struct *clistr_chk =
		client->_structures+i;

	      if (strcmp(name_id, clistr_chk->_id) == 0)
		{
		  clistr = clistr_chk;
		  if (struct_id)
		    *struct_id = i;
		  break;
		}
	    }
	}

      if (clistr == NULL)
	{
	  client->_last_error = "Requested structure (ntuple) "
	    "not existing from server.";
	  errno = ENOMSG;
	  return -1;
	}

      if (clistr->_dest_struct_size)
	{
	  client->_last_error = "Requested structure (ntuple) "
	    "already mapped.";
	  errno = EBUSY;
	  return -1;
	}
    }

  /* */

  if (slo)
    {
      /* Check that the struct_layout_info is good.
       */

      size_t expect_size = sizeof (struct ext_data_structure_layout) +
	((slo->_num_items-1) * sizeof (struct ext_data_structure_layout_item))+
	slo->_pack_list_items * sizeof(uint32_t);

      /* The structure total size will also be aligned to the size of
       * a pointer, so account for that.
       */

      size_t expect_align = sizeof (const char *); /* Will be 2^n */

      expect_size = (expect_size + expect_align - 1) & ~(expect_align - 1);

      if (slo->_magic != EXTERNAL_WRITER_MAGIC ||
	  slo->_size_info != size_info ||
	  slo->_size_struct_onion != slo->_size_struct ||
	  slo->_num_items < 1 ||
	  slo->_size_info != expect_size)
	{
	  /*
	  fprintf(stderr,"err %x (%d %zd) (%d %zd %zd) (%d %zd) "
		  "[%zd %zd %zd]\n",
		  slo->_magic,slo->_size_info,size_info,
		  slo->_size_struct,size_buf,size_buf,
		  slo->_size_info,expect_size,
		  sizeof (struct ext_data_structure_layout),
		  sizeof (struct ext_data_structure_layout_item),
		  sizeof(uint32_t));
	  */
	  client->_last_error = "Structure layout information inconsistent.";
	  errno = EINVAL;
	  return -1;
	}

      if (slo->_size_struct != size_buf)
	{
	  client->_last_error =
	    "Structure layout gives different buffer size.";
	  errno = EINVAL;
	  return -1;
	}

      if (slo->_items[0]._offset != 0 ||
	  slo->_items[0]._size != size_buf)
	{
	  // fprintf(stderr,"err 2\n");
	  client->_last_error =
	    "Structure layout item 0 information inconsistent.";
	  errno = EINVAL;
	  return -1;
	}

      for (i = 0; i < slo->_num_items; i++)
	{
	  if (slo->_items[i]._offset > size_buf ||
	      slo->_items[i]._offset + slo->_items[i]._size > size_buf)
	    {
	      // fprintf(stderr,"err 3\n");
	      client->_last_error =
		"Structure layout item information inconsistent.";
	      errno = EINVAL;
	      return -1;
	    }
	}

      /* Make our own copy of the pack list (in case the client
       * program just had it as a temporary variable).
       */

      clistr->_dest_pack_list =
	(uint32_t *) malloc (slo->_pack_list_items * sizeof(uint32_t));

      if (!clistr->_dest_pack_list)
	{
	  client->_last_error = "Memory allocation failure (dest pack list).";
	  errno = ENOMEM;
	  return -1;
	}

      clistr->_dest_pack_list_end =
	clistr->_dest_pack_list + slo->_pack_list_items;

      slo_items =
	((const struct ext_data_structure_layout_item *) (slo + 1)) - 1;
      slo_pack_list = (const uint32_t *) (slo_items + slo->_num_items);

      memcpy(clistr->_dest_pack_list,slo_pack_list,
	     slo->_pack_list_items * sizeof(uint32_t));

      /* Make the reverse list, to be able to quickly (directly) look any
       * controlling items up.  (Needed for clearing).  Also calculate the
       * worst case message size.
       */

      clistr->_dest_reverse_pack =
	(uint32_t *) malloc (size_buf * sizeof(uint32_t));

      if (!clistr->_dest_reverse_pack)
	{
	  client->_last_error = "Memory allocation failure (reverse pack).";
	  errno = ENOMEM;
	  return -1;
	}

      memset(clistr->_dest_reverse_pack,0,size_buf * sizeof(uint32_t));

      // Loop over the entire offset buffer...

      {
	uint32_t *o    = clistr->_dest_pack_list;
	uint32_t *oend = clistr->_dest_pack_list_end;

	clistr->_dest_max_pack_items = 0;
	clistr->_dest_static_pack_items = 0;

	while (o < oend)
	  {
	    uint32_t mark   = *(o++);
	    uint32_t offset = *(o++);
	    uint32_t loop = mark & EXTERNAL_WRITER_MARK_LOOP;

	    clistr->_dest_static_pack_items++;

	    if (loop)
	      {
		uint32_t max_loops = *(o++);
		uint32_t loop_size = *(o++);

		uint32_t items = max_loops * loop_size;

		uint32_t *onext = o + 2 * items;

		clistr->_dest_max_pack_items += items;

		clistr->_dest_reverse_pack[offset] =
		  (uint32_t) (o - clistr->_dest_pack_list);

		o = onext;
	      }
	  }
	clistr->_dest_max_pack_items += clistr->_dest_static_pack_items;
      }
    }

  clistr->_dest_struct_size = size_buf;

  if (client->_state == EXT_DATA_STATE_OPEN_OUT)
    {
      struct external_writer_buf_header *header;
      uint32_t *p;

      /* Without a pack list (i.e. without slo (struct_layout_info)),
       * we cannot write data, as the pack order is not known.
       */

      if (!slo)
	{
	  client->_last_error = "Cannot write without structure layout info.";
	  errno = EFAULT;
	  return -1;
	}

      /* We need to allocate the write buffer.
       *
       * The size is limited by the pack list.  Plus the message header.
       */

      size_t bufsize = clistr->_dest_max_pack_items * sizeof(uint32_t) +
	sizeof(struct external_writer_buf_header) + 2 * sizeof(uint32_t);

      /* The other messages we send first are fixed length, and
       * contained within the 4kB buffer size.
       */

      bufsize = (bufsize + 0x1000-(uint32_t) 1) & ~(0x1000-(uint32_t) 1);

      client->_buf = (char *) malloc (bufsize);

      if (!client->_buf)
	{
	  client->_last_error = "Memory allocation failure (buf).";
	  errno = ENOMEM;
	  return -1;
	}

      client->_buf_alloc = bufsize;

      /* We send some minimum messages to the recipient so that it at
       * least can check that the data is not completely bogus.
       */

      // EXTERNAL_WRITER_BUF_OPEN_FILE   /* magic */

      header = (struct external_writer_buf_header *) client->_buf;

      header->_request = htonl(EXTERNAL_WRITER_BUF_OPEN_FILE |
			       EXTERNAL_WRITER_REQUEST_HI_MAGIC);
      p = (uint32_t *) (header+1);
      *(p++) = htonl(EXTERNAL_WRITER_MAGIC);
      *(p++) = htonl(0);
      header->_length = htonl((uint32_t) (((char *) p) - ((char *) header)));

      // EXTERNAL_WRITER_BUF_BOOK_NTUPLE /* size */

      header = (struct external_writer_buf_header *) p;

      header->_request = htonl(EXTERNAL_WRITER_BUF_BOOK_NTUPLE |
			       EXTERNAL_WRITER_REQUEST_HI_MAGIC);
      p = (uint32_t *) (header+1);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      *(p++) = htonl(0);
      header->_length = htonl((uint32_t) (((char *) p) - ((char *) header)));

      // EXTERNAL_WRITER_BUF_ALLOC_ARRAY /* size */

      header = (struct external_writer_buf_header *) p;

      header->_request = htonl(EXTERNAL_WRITER_BUF_ALLOC_ARRAY |
			       EXTERNAL_WRITER_REQUEST_HI_MAGIC);
      p = (uint32_t *) (header+1);
      *(p++) = htonl((uint32_t) clistr->_dest_struct_size);
      header->_length = htonl((uint32_t) (((char *) p) - ((char *) header)));

      // EXTERNAL_WRITER_BUF_RESIZE      /* tell size? */

      header = (struct external_writer_buf_header *) p;

      header->_request = htonl(EXTERNAL_WRITER_BUF_RESIZE |
			       EXTERNAL_WRITER_REQUEST_HI_MAGIC);
      p = (uint32_t *) (header+1);
      *(p++) = htonl((uint32_t) bufsize);
      *(p++) = htonl(EXTERNAL_WRITER_MAGIC);
      header->_length = htonl((uint32_t) (((char *) p) - ((char *) header)));

      // EXTERNAL_WRITER_BUF_ARRAY_OFFSETS

      header = (struct external_writer_buf_header *) p;

      header->_request = htonl(EXTERNAL_WRITER_BUF_ARRAY_OFFSETS |
			       EXTERNAL_WRITER_REQUEST_HI_MAGIC);
      p = (uint32_t *) (header+1);
      *(p++) = htonl(slo->_items[0]._xor);
      header->_length = htonl((uint32_t) (((char *) p) - ((char *) header)));

      // EXTERNAL_WRITER_BUF_SETUP_DONE_WR

      header = (struct external_writer_buf_header *) p;

      header->_request = htonl(EXTERNAL_WRITER_BUF_SETUP_DONE_WR |
			       EXTERNAL_WRITER_REQUEST_HI_MAGIC);
      p = (uint32_t *) (header+1);
      header->_length = htonl((uint32_t) (((char *) p) - ((char *) header)));

      // And then dump it to the reader - after this, we'll start to
      // write data...

      client->_buf_filled = (size_t) (((char *) p) - ((char *) client->_buf));

      /* It's ok to start writing data. */
      client->_state = EXT_DATA_STATE_SETUP_WRITE;

      if (ext_data_flush_buffer(client) != 0)
	return -1; // errno already set

      return 0;
    }

  /* Before we're happy with the server, we want to see the magic and
   * the setup done event, so that the structure xor_sum can be
   * verified.
   */

  /* Now do the checking. */

  if (!struct_info &&
      clistr->_dest_struct_size != clistr->_orig_struct_size)
    {
      client->_last_error =
	"Bad alloc message struct size during setup.";
      errno = EPROTO;
      return -1;
    }

  if (slo)
    {
      /*
      fprintf (stderr,
	       "xor: %x vx %x\n",
	       slo->_items[0]._xor,clistr->_orig_xor_sum_msg);
      */
      if (slo->_items[0]._xor != clistr->_orig_xor_sum_msg)
	{
	  /*
	  fprintf(stderr,"mismatch %x vs %x\n",
		  slo->_items[0]._xor, clistr->_orig_xor_sum_msg);
	  */
	  client->_last_error =
	    "Bad setup done message xor vs. provided during setup.";
	  errno = EPROTO;
	  return -1;
	}
    }

  if (struct_info)
    {
      int ret;
      int all_to_same_from;
      uint32_t map_success;

      /* Create mapping between the two structures. */

      struct_info->_server_struct_info = clistr->_struct_info_msg;
      if (!struct_info->_items)
      {
	struct_info->_items = ext_data_structure_item_copy(clistr->_struct_info_msg->_items);
        clistr->_dest_struct_size = clistr->_orig_struct_size;
      }
      ret = ext_data_struct_match_items(client, clistr,
					clistr->_struct_info_msg,
					struct_info,
					&all_to_same_from,
					&map_success);

      if (ret)
	return ret; /* -1 ? */

      if (struct_map_success)
	*struct_map_success = map_success;

      /* We as an optimisation do *not* use the temporary
       * orig_array buffer if we have an exact structure
       * match.  It must also match in size.
       */

      if (!all_to_same_from ||
	  clistr->_orig_struct_size != clistr->_dest_struct_size)
	{
	  clistr->_orig_array = malloc (clistr->_orig_struct_size);

          printf("all_to_same_from = %d\n", all_to_same_from);
	  if (clistr->_orig_array == NULL)
	    {
	      client->_last_error =
		"Memory allocation failure (orig array).";
	      errno = ENOMEM;
	      return -1;
	    }
	}
        printf("Array size: %ld\n", clistr->_dest_struct_size);
    }

  /* It's ok to read data. */
  client->_state = EXT_DATA_STATE_SETUP_READ;
  
  return 0;
}

/* This function is for internal use.  It is shared with the
 * struct_writer.  Returns 0 on success.
 */

int ext_data_write_bitpacked_event(char *dest,size_t dest_size,
				   uint8_t *src,uint8_t *end_src)
{
  uint32_t offset = 0;

  for ( ; src < end_src; )
    {
      int shift_offset = 2;
      uint8_t v;
      uint32_t value = 0; // to make compiler happy

      v = *(src++);

      while (v & 0x80)
	{
	  offset += ((uint32_t) (v & 0x7f)) << shift_offset;
	  shift_offset += 7;
	  if (src >= end_src) // unlikely
	    return -1;
	  v = *(src++);
	}

      switch (v >> 5)
	{
	case 0:
	  value = (uint32_t) (v & 0x1f);
	  //fprintf(stderr,"WBP:0: @ 0x%08x : 0x%08x\n", offset, v & 0x1f);
	  break;
	case 1:
	{
	  uint32_t high = ((uint32_t) (v & 0x1f)) << 8;
	  if (src >= end_src) // unlikely
	    return -2;
	  value = high | *(src++);
	  //fprintf(stderr,"WBP:1: @ 0x%08x : 0x%08x\n", offset, value);
	  break;
	}
	case 2:
	{
	  offset += ((uint32_t) (v & 0x1f)) << shift_offset;
	  if (src+3 >= end_src) // unlikely
	    return -3;
	  value = ((uint32_t) *(src++)) << 24;
	  value |= (uint32_t) (*(src++) << 16);
	  value |= (uint32_t) (*(src++) << 8 );
	  value |=             *(src++);
	  //fprintf(stderr,"WBP:2: @ 0x%08x : 0x%08x\n", offset, value);
	  break;
	}
	case 3:
	  offset += ((uint32_t) (v & 0x0f)) << shift_offset;
	  // Next is a trick, if *(src++) & 0x10,
	  // the value will be shifted out -> 0 remains
	  value = (uint32_t) 0x7fc00000 << (v & 0x10);
	  //fprintf(stderr,"WBP:3: @ 0x%08x : 0x%08x\n",
	  //	  offset, (uint32_t) 0x7fc00000 << (v & 0x10));
	  break;
	}

      if (offset + sizeof(uint32_t) > dest_size) // unlikely
	return -4;
      // By design, we are always aligned...
      // if (offset & 3) // unlikely
      //   return -5;

      *((uint32_t *) (dest + offset)) = value;

      offset += (uint32_t) sizeof(uint32_t);
    }

  if (offset != dest_size) // unlikely
    return -6;

  return 0;
}

/* This function is for internal use.  It is similar to the code in
 * the struct_writer.  Returns 0 on success.
 */

int ext_data_write_packed_event(struct ext_data_client *client,
				char *dest,
				int struct_id,
				uint32_t *src,uint32_t *end_src)
{
  const struct ext_data_client_struct *clistr;

  uint32_t *p    = src;
  uint32_t *pend = end_src;

  uint32_t *o;
  uint32_t *oend;

  if (struct_id >= client->_num_structures)
    return -5;

  clistr = &client->_structures[struct_id];

  o    = clistr->_orig_pack_list;
  oend = clistr->_orig_pack_list_end;

  /*
  fprintf (stderr, "[str: %d] %zd cmp %zd\n",
	   struct_id,
	   pend - p,
	   (ssize_t) clistr->_orig_static_pack_items);
  */
  
  if (pend - p < (ssize_t) clistr->_orig_static_pack_items)
    return -1;

  uint32_t *pcheck = p + clistr->_orig_static_pack_items;

  while (o < oend)
    {
      uint32_t mark   = *(o++);
      uint32_t offset = *(o++);
      uint32_t loop = mark & EXTERNAL_WRITER_MARK_LOOP;
      uint32_t value = ntohl(*(p++));

      *((uint32_t *) (dest + offset)) = value;

      if (loop)
	{
	  uint32_t max_loops = *(o++);
	  uint32_t loop_size = *(o++);

	  uint32_t *onext = o + 2 * max_loops * loop_size;

	  if (value > max_loops)
	    return -2;

	  uint32_t items = value * loop_size;
	  uint32_t i;

	  if (pend - pcheck < (ssize_t) items)
	    return -3;

	  pcheck += items;

	  for (i = items; i; i--)
	    {
	      mark   = *(o++);
	      offset = *(o++);
	      value = ntohl(*(p++));

	      *((uint32_t *) (dest + offset)) = value;
	    }

	  o = onext;
	}
    }

  if (p != pend)
    return -4;

  return 0;
}


/* Handle messages up to the next ntuple fill event. */
static int
ext_data_fetch_event_message(struct ext_data_client *client,
			     struct external_writer_buf_header **get_header,
			     uint32_t *struct_index)
{
  /* Data read from the source until we have an entire message. */
  struct external_writer_buf_header *header;

  header = ext_data_peek_message(client);

  *get_header = header;

  if (header == NULL)
    return -1; /* errno already set */

  uint32_t length = ntohl(header->_length);

  switch (ntohl(header->_request) & EXTERNAL_WRITER_REQUEST_LO_MASK)
    {
    case EXTERNAL_WRITER_BUF_DONE:
    case EXTERNAL_WRITER_BUF_ABORT:
      /* In either case, we're out of data.  Not consumed. */
      return 0;

    case EXTERNAL_WRITER_BUF_NAMED_STRING:
      /* This one should be part of the init, i.e. not appear here. */
      errno = EPROTO;
      client->_last_error = "Named string in fetch, after setup.";
      return -1;

    default:
      /* Unexpected message, not allowed.  Not consumed. */
      errno = EPROTO;
      client->_last_error = "Unexpected message in fetch.";
      return -1;

    case EXTERNAL_WRITER_BUF_NTUPLE_FILL:
      {
	uint32_t *p = (uint32_t *) (header+1);
	uint32_t *end = (uint32_t *) (((char*) header) + length);

	if (p + (client->_sort_u32_words + 3) > end)
	  {
	    client->_last_error = "Event message too short for headers.";
	    errno = EBADMSG;
	    return -1;
	  }

	p += client->_sort_u32_words;

	*struct_index = ntohl(*(p++));

	if (*struct_index >= (uint32_t) client->_num_structures)
	  {
	    client->_last_error = "Event structure index outside bounds.";
	    errno = EBADMSG;
	    return -1;
	  }

	return 1;
      }
    }
}

int ext_data_next_event(struct ext_data_client *client,
			int *struct_id)
{
  uint32_t struct_index;

  if (!client)
    {
      /* client->_last_error = "Client context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (client->_state != EXT_DATA_STATE_SETUP_READ)
    {
      client->_last_error = "Client context has not had setup (for reading).";
      errno = EFAULT;
      return -1;
    }

  for ( ; ; )
    {
      struct external_writer_buf_header *header;

      int ret = ext_data_fetch_event_message(client, &header, &struct_index);

      if (ret != 1)
	return ret;

      if (client->_fetched_event)
	{
	  /* We shall discard one event first. */

	  uint32_t length = ntohl(header->_length);

	  client->_buf_used += length;

	  client->_fetched_event = 0;
	  continue;
	}
      break;
    }

  *struct_id = (int) struct_index;

  client->_fetched_event = 1;

  return 1;
}

int ext_data_fetch_event(struct ext_data_client *client,
			 void *buf,size_t size
#if !STRUCT_WRITER
			 ,int struct_id
#endif
#if STRUCT_WRITER
			 ,struct external_writer_buf_header **header_in
			 ,uint32_t *length_in
#endif
			 )
{
  const struct ext_data_client_struct *clistr;
#if STRUCT_WRITER
  int struct_id = 0; /* fix to accept whatever event, call next_event */
#endif

  /* Data read from the source until we have an entire message. */
  struct external_writer_buf_header *header;

  if (!client)
    {
      /* client->_last_error = "Client context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (client->_state != EXT_DATA_STATE_SETUP_READ)
    {
      client->_last_error = "Client context has not had setup (for reading).";
      errno = EFAULT;
      return -1;
    }

  if (struct_id < 0 || struct_id >= client->_num_structures)
    {
      client->_last_error = "Request for non-existing structure index (key).";
      errno = EINVAL;
      return -1;
    }  

  clistr = &client->_structures[struct_id];

  if (size != clistr->_dest_struct_size)
    {
      client->_last_error = "Buffer size mismatch.";
      errno = EINVAL;
      return -1;
    }

  client->_raw_ptr = NULL;
  client->_raw_words = 0;

  /* So, try to treat the message.
   *
   * Note that we ignore most messages, and only partially treat some.
   */

  for ( ; ; )
    {
      uint32_t struct_index = -1; /* make compiler happy */

      int ret = ext_data_fetch_event_message(client, &header, &struct_index);

      uint32_t length = ntohl(header->_length);

      if (ret == 0)
	{
#ifdef STRUCT_WRITER

	  *header_in = header;
	  *length_in = length;
#endif
	  return 0;
	}

      if (ret != 1)
	return ret;

      if (struct_index != (uint32_t) struct_id)
	{
	  /* Discard this event. */
	  client->_buf_used += length;
	  continue;
	}

      /* This event is for us. */
      break;
    }

  /* We do however make sure that (given correctness of the buf and
   * size parameters, this function can never crash on bad network
   * input, but rather produces some error message).
   */

  uint32_t length = ntohl(header->_length);

  assert ((ntohl(header->_request) & EXTERNAL_WRITER_REQUEST_LO_MASK) ==
	  EXTERNAL_WRITER_BUF_NTUPLE_FILL);
  
  {
    /* The main message, prompting fill of the structure.
     */
#if 0
    uint32_t *p   = (uint32_t *) (header+1);
    uint32_t *end = (uint32_t *) (((char*) header) +
				  ntohl(header->_length));
#endif

    uint32_t *p = (uint32_t *) (header+1);
    uint32_t *end = (uint32_t *) (((char*) header) + length);
    uint8_t *start;
    uint32_t struct_index;
    uint32_t ntuple_index;
    uint32_t marker, compact_marker, real_len;

    char *unpack_buf = (char *) buf;
    size_t unpack_size = size;

    if (p + (client->_sort_u32_words + 3) > end)
      {
	client->_last_error = "Event message too short for headers.";
	errno = EBADMSG;
	return -1;
      }

    p += client->_sort_u32_words;

    struct_index = ntohl(*(p++));
    ntuple_index = ntohl(*(p++));

    // printf ("index: %d\n",ntuple_index);

    (void) struct_index; /* Handled above. */

    if (ntuple_index != 0)
      {
	client->_last_error = "Non-zero ntuple_index - "
	  "do not know how to handle.";
	/* Or rather, do not know if it is properly propagated. */
	/* Especially to a struct_writer continuation server. */
	errno = EBADMSG;
	return -1;
      }

    if (clistr->_orig_array)
      {
	unpack_buf = (char *) clistr->_orig_array;
	unpack_size = clistr->_orig_struct_size;
      }

    if (clistr->_max_raw_words)
      {
	client->_raw_words = ntohl(*(p++));

	if (p + (client->_raw_words + 1) > end)
	  {
	    client->_last_error = "Event message too short for raw data.";
	    errno = EBADMSG;
	    return -1;
	  }

	client->_raw_ptr = p;
	p += client->_raw_words;
      }

    marker = ntohl(*(p++));
    compact_marker = marker & (EXTERNAL_WRITER_COMPACT_PACKED |
			       EXTERNAL_WRITER_COMPACT_NONPACKED);

    if (compact_marker != EXTERNAL_WRITER_COMPACT_PACKED &&
	compact_marker != EXTERNAL_WRITER_COMPACT_NONPACKED)
      {
	client->_last_error = "Compact marker invalid.";
	errno = EBADMSG;
	return -1;
      }

    if (compact_marker & EXTERNAL_WRITER_COMPACT_NONPACKED)
      {
	int ret;
	
	/* It is not bit-packed.  Use the pack list.
	 */

	client->_buf_used += length;

#ifdef STRUCT_WRITER
	/* We actually do not want to get it unpacked for us.  We will
	 * handle it ourselves.
	 */
	*header_in = header;
	*length_in = length;
	return 2;
#endif

	ret = ext_data_write_packed_event(client,unpack_buf,
					  struct_index,p,end);

	if (ret)
	  {
	    /* fprintf (stderr, "ret=%d\n", ret); */
	    client->_last_error = "Event message unpack failure.";
	    errno = EBADMSG;
	    return -1;
	  }
      }
    else
      {
	int ret;

	real_len = marker & 0x3fffffff;

	if ((end - p) * sizeof (uint32_t) !=
	    ((real_len + 3) & (uint32_t) ~3))
	  {
	    client->_last_error = "Event message packed length mismatch.";
	    errno = EBADMSG;
	    return -1;
	  }

	/* Either we strike an error or not, we declare this event
	 * as consumed.  Note: there is not much sense for clients
	 * to continue, and they can anyhow not distinguish the
	 * error message from other (more fatal) failures that give
	 * the same error code.
	 */

	client->_buf_used += length;

	start = (uint8_t *) p;

	ret = ext_data_write_bitpacked_event(unpack_buf,
					     unpack_size,
					     start,start + real_len);

	if (ret)
	  {
	    client->_last_error = "Event message bit-unpack failure.";
	    errno = EBADMSG;
	    return -1;
	  }
      }

    if (clistr->_orig_array)
      {
	ext_data_struct_map_items(clistr,
				  (char *) buf,
				  (char *) unpack_buf);
      }

    client->_fetched_event = 0;
    /* We got an event! */
    return 1;
  }
}

int ext_data_get_raw_data(struct ext_data_client *client,
                          const void **raw,ssize_t *raw_words)
{
  if (!client)
    {
      /* client->_last_error = "Client context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (raw == NULL || raw_words == NULL)
    {
      client->_last_error = "Bad raw or raw_words pointer.";
      errno = EINVAL;
      return -1;
    }

  /* Do we need to swap the data? */
  if ((ntohl(0x01020304) != 0x01020304) && /* should be static by compiler */
      client->_raw_ptr &&
      client->_raw_ptr != client->_raw_swapped)
    {
      uint32_t *d32 = client->_raw_swapped;
      uint32_t *s32 = client->_raw_ptr;
      uint32_t i;

      /* One could of course discuss whether data should instead be
       * sent in native order and then byteswapped only when necessary
       * instead of insisting on network order.  Measurements of
       * 32-bit byte-swap speed on some modern CPUs:
       *
       * Opteron270 (2.0 GHz)   496 MiB/s
       * E3-1260L   (3.3 GHz)  1294 MiB/s
       * E3-1240v3  (3.8 GHz)  1985 MiB/s
       */

      for (i = 0; i < client->_raw_words; i++)
	*(d32++) = ntohl(*(s32++));

      client->_raw_ptr = client->_raw_swapped;
    }

  *raw = client->_raw_ptr;
  *raw_words = client->_raw_words;
  return 0;
}

int ext_data_clear_event(struct ext_data_client *client,
			 void *buf,size_t size,int clear_zzp_lists,
			 int struct_id)
{
  const struct ext_data_client_struct *clistr;

  uint32_t *o, *oend;
  char *b;

  /* Data read from the source until we have an entire message. */

  if (!client)
    {
      /* client->_last_error = "Client context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (client->_state != EXT_DATA_STATE_SETUP_READ &&
      client->_state != EXT_DATA_STATE_SETUP_WRITE)
    {
      client->_last_error = "Client context has not had setup.";
      errno = EFAULT;
      return -1;
    }

  if (struct_id < 0 || struct_id >= client->_num_structures)
    {
      client->_last_error = "Request for non-existing structure index (key).";
      errno = EINVAL;
      return -1;
    }  

  clistr = &client->_structures[struct_id];

  if (size != clistr->_dest_struct_size)
    {
      client->_last_error = "Buffer size mismatch.";
      errno = EINVAL;
      return -1;
    }

  if (!clistr->_dest_pack_list)
    {
      /* Setup was called without struct_layout_info (i.e. only with
       * struct_info) - we have no pack list, so do not know where
       * the items are.
       *
       * In principle, we could produce such a list from the
       * struct_info.
       */
      client->_last_error = "No pack list known - cannot clear.";
      errno = EFAULT;
      return -1;
    }

  /* Run through the offset list and clean the data structure.
   */

  o    = clistr->_dest_pack_list;
  oend = clistr->_dest_pack_list_end;

  b = (char*) buf;

  while (o < oend)
    {
      uint32_t mark   = *(o++);
      uint32_t offset = *(o++);
      uint32_t loop = mark & EXTERNAL_WRITER_MARK_LOOP;
      uint32_t clear_zero =
	mark & EXTERNAL_WRITER_MARK_CLEAR_ZERO;

      // Dirty trick with the NaN clearing, to avoid branching.  If
      // marker (bit 30) shifted down to bit 4, i.e. value 16 is set,
      // then we'll shift the NaN bits out and the value used for
      // clearing will be 0.
      (*((uint32_t *) (b + offset))) =
	(uint32_t) 0x7fc00000 << (clear_zero >> 26);

      if (loop)
	{
	  // It's a loop control.  Make sure the value was within
	  // limits.

	  uint32_t max_loops = *(o++);
	  uint32_t loop_size = *(o++);

	  uint32_t *onext = o + 2 * max_loops * loop_size;

	  if (clear_zzp_lists)
	    {
	      uint32_t items = max_loops * loop_size;
	      uint32_t i;

	      for (i = items; i; i--)
		{
		  uint32_t item_mark = *(o++);
		  uint32_t item_offset = *(o++);
		  uint32_t item_clear_zero =
		    item_mark & EXTERNAL_WRITER_MARK_CLEAR_ZERO;

		  // Dirty trick (nan <-> zero)...
		  (*((uint32_t *) (b + item_offset))) =
		    (uint32_t) 0x7fc00000 << (item_clear_zero >> 26);
		}
	    }
	  o = onext;
	}
    }
  return 0;
}



void ext_data_clear_zzp_lists(struct ext_data_client *client,
			      void *buf,void *item,
			      int struct_id)
{
  const struct ext_data_client_struct *clistr;

  uint32_t *o;
  char *b;
  uint32_t value;
  uint32_t item_info;
  int item_offset;

  // assert (client);
  // assert (client->_setup);
  // assert (buf);
  // assert (item);

  // This routine will be called so often (and has so little to do)
  // that no error checking is performed.  It is presumed that having
  // the error checking in ext_data_clear_event is enough

  // The @item is supposed to be an control item.  Use our direct
  // look-up table for the control items to find the associated offset
  // item in the pack list.  Then clear so many values.

  clistr = &client->_structures[struct_id];

  b = (char*) buf;

  item_offset = (int) ((char *) item - (char *) buf);

  item_info = clistr->_dest_reverse_pack[item_offset];

  o = clistr->_dest_pack_list + item_info;

  value = *((uint32_t *) item);

  // It's a loop control.  Make sure the value was within
  // limits.

  uint32_t max_loops = *(o++);
  uint32_t loop_size = *(o++);

  // This will put an end to things!
  if (value > max_loops)
    {
      // Just clamp the value here.  The write / pack routine will
      // also detect the error, and return it as a fault.

      // No use in setting error message, as we will not report it.
      // client->_last_error = "Array ctrl item value out of bounds.";

      value = max_loops;
    }

  uint32_t items = value * loop_size;
  uint32_t i;

  for (i = items; i; i--)
    {
      uint32_t mark   = *(o++);
      uint32_t offset = *(o++);
      uint32_t clear_zero =
	mark & EXTERNAL_WRITER_MARK_CLEAR_ZERO;

      // Dirty trick (nan <-> zero)...
      (*((uint32_t *) (b + offset))) =
	(uint32_t) 0x7fc00000 << (clear_zero >> 26);
    }
}


int ext_data_write_event(struct ext_data_client *client,
			 void *buf,size_t size)
{
  const struct ext_data_client_struct *clistr;

  /* Data read from the source until we have an entire message. */

  struct external_writer_buf_header *header;
  uint32_t *cur;
  uint32_t length;
  uint32_t *o, *oend;
  char *b;

  int struct_id = 0;

  if (!client)
    {
      /* client->_last_error = "Client context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (client->_state != EXT_DATA_STATE_SETUP_WRITE)
    {
      client->_last_error = "Client context has not had setup (for writing).";
      errno = EFAULT;
      return -1;
    }

  if (struct_id >= client->_num_structures)
    {
      errno = EFAULT;
      return -1;
    }

  clistr = &client->_structures[struct_id];

  if (size != clistr->_dest_struct_size)
    {
      client->_last_error = "Buffer size mismatch.";
      errno = EINVAL;
      return -1;
    }

  /* Is there enough space in the buffer to hold a worst case event?
   */

  if (client->_buf_alloc - client->_buf_filled <
      sizeof (struct external_writer_buf_header) + 3 * sizeof(uint32_t) +
      clistr->_dest_max_pack_items * sizeof(uint32_t))
    {
      if (ext_data_flush_buffer(client))
	return -1; // errno has been set
    }

  header =
    (struct external_writer_buf_header *) (client->_buf + client->_buf_filled);

  cur = (uint32_t *) (header + 1);

  *(cur++) = htonl(0); /* struct_index */
  *(cur++) = htonl(0); /* ntuple_index */
  *(cur++) = htonl(EXTERNAL_WRITER_COMPACT_NONPACKED);

  /* Run through the offset list and write the data to the buffer.
   */

  o    = clistr->_dest_pack_list;
  oend = clistr->_dest_pack_list_end;

  b = (char*) buf;

  while (o < oend)
    {
      uint32_t mark   = *(o++);
      uint32_t offset = *(o++);
      uint32_t loop = mark & EXTERNAL_WRITER_MARK_LOOP;
      // uint32_t clear_nan_zero = mark & EXTERNAL_WRITER_MARK_CLEAR_NAN;

      uint32_t value = (*((uint32_t *) (b + offset)));

      *(cur++) = htonl(value);

      if (loop)
	{
	  // It's a loop control.  Make sure the value was within
	  // limits.

	  uint32_t max_loops = *(o++);
	  uint32_t loop_size = *(o++);

	  if (value > max_loops)
	    {
	      // Error!
	      client->_last_error = "Array ctrl item value out of bounds.";
	      return (int) offset;
	    }

	  uint32_t *onext = o + 2 * max_loops * loop_size;
	  uint32_t items = value * loop_size;
	  uint32_t i;

	  for (i = items; i; i--)
	    {
	      uint32_t item_mark   = *(o++);
	      uint32_t item_offset = *(o++);
	      // uint32_t item_clear_nan_zero =
	      //   item_mark & EXTERNAL_WRITER_MARK_CLEAR_NAN;

	      (void) item_mark;

	      value = (*((uint32_t *) (b + item_offset)));

	      *(cur++) = htonl(value);
	    }
	  o = onext;
	}
    }

  length = (uint32_t) (((char*) cur) - ((char*) header));

  header->_request = htonl(EXTERNAL_WRITER_BUF_NTUPLE_FILL |
			   EXTERNAL_WRITER_REQUEST_HI_MAGIC);
  header->_length = htonl(length);

  /* fprintf (stderr, "wrote: %d\n", length); */

  client->_buf_filled += length;

  return (int) size; // success
}

int ext_data_flush_buffer(struct ext_data_client *client)
{
  if (!client)
    {
      /* client->_last_error = "Client context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (client->_state != EXT_DATA_STATE_SETUP_WRITE)
    {
      client->_last_error = "Client context has not had setup (for writing).";
      errno = EFAULT;
      return -1;
    }

  // Write any data available in the buffer.

  size_t left = client->_buf_filled;
  char *p = client->_buf;

  while (left)
    {
      ssize_t n =
	write(client->_fd,p,left);

      if (n == 0)
	{
	  // Cannot happen?  (should have gotten EPIPE)
	  client->_last_error = "Failure writing buffer data (ret 0).";
	  errno = EPROTO;
	  return -1;
	}

      if (n == -1)
	{
	  if (errno == EINTR)
	    continue;
	  client->_last_error = "Failure writing buffer data.";
	  /* Failure. */
	  return -1;
	}

      left -= (size_t) n;
    }

  client->_buf_filled = 0;
  return 0;
}

int ext_data_close(struct ext_data_client *client)
{
  if (!client)
    {
      /* client->_last_error = "Client context NULL."; */
      errno = EFAULT;
      return -1;
    }

  if (client->_state == EXT_DATA_STATE_SETUP_WRITE)
    {
      struct external_writer_buf_header *header;

      /* Flush whatever data is still in the buffers.  We do the flush
       * with the real data to ensure space in the buffer for the done
       * message.
       */

      if (ext_data_flush_buffer(client) != 0)
	return -1; // errno already set

      /* Then, we should write a done message, and flush again. */

      header =
	(struct external_writer_buf_header *) (client->_buf +
					       client->_buf_filled);

      header->_request = htonl(EXTERNAL_WRITER_BUF_DONE |
			       EXTERNAL_WRITER_REQUEST_HI_MAGIC);
      header->_length = htonl(sizeof(struct external_writer_buf_header));

      client->_buf_filled += sizeof(struct external_writer_buf_header);

      if (ext_data_flush_buffer(client) != 0)
	return -1; // errno already set
    }

  /* Only close the file handle if opened by us. */

  while (client->_fd_close != -1)
    {
      if (close(client->_fd_close) == 0)
	break;

      if (errno == EINTR)
	continue;

      client->_last_error = "Failure closing socket.";
      int errsv = errno;
      /* Cannot fail, could possibly change errno? */
      ext_data_free(client);
      errno = errsv;
      return -1;
    }

  /* Cannot fail. */
  ext_data_free(client);

  return 0;
}

void ext_data_rand_fill(void *buf,size_t size)
{
  char *p = (char*) buf;
  size_t i;

  static unsigned long next = 123456789;

  for (i = size; i; --i)
    {
      next = next * 1103515245 + 12345;
      *(p++) = (char) ((unsigned)(next/65536) % 32768);
    }
}

const char *ext_data_last_error(struct ext_data_client *client)
{
  if (client == NULL)
    return NULL;

  return client->_last_error;
}

/* Functions that 'handle' errors by printing messages to stderr.
 */

#ifndef STRUCT_WRITER

#include <stdio.h>

struct ext_data_structure_info *ext_data_struct_info_alloc_stderr()
{
  struct ext_data_structure_info *struct_info =
    ext_data_struct_info_alloc();

  if (struct_info == NULL)
    {
      perror("ext_data_struct_info_alloc");
      return NULL;
    }

  return struct_info;
}

int ext_data_struct_info_item_stderr(struct ext_data_structure_info *struct_info,
				     size_t offset, size_t size,
				     int type,
				     const char *prename, int preindex,
				     const char *name, const char *ctrl_name,
				     int limit_max, uint32_t flags)
{
  int ret = ext_data_struct_info_item(struct_info,
				      offset, size, type,
				      prename, preindex,
				      name, ctrl_name,
				      limit_max, flags);

  if (ret != 0)
    {
      perror("ext_data_struct_info_item");
      fprintf (stderr,"Errors for item '%s' @ 0x%x: %s\n",
	       name, (int) offset,
	       struct_info->_last_error);
      return 0;
    }

  return 1;
}

struct ext_data_client *ext_data_connect_stderr(const char *server)
{
  struct ext_data_client *client =
    ext_data_connect(server);

  if (client == NULL)
    {
      perror("ext_data_connect");
      fprintf (stderr,"Failed to connect to server '%s'.\n",server);
      return NULL;
    }

  fprintf (stderr,"Connected to server '%s'.\n",server);

  return client;
}

int ext_data_setup_stderr(struct ext_data_client *client,
			  const void *struct_layout_info,
			  size_t size_info,
			  struct ext_data_structure_info *struct_info,
			  uint32_t *struct_map_success,
			  size_t size_buf,
			  const char *name_id, int *struct_id,
			  uint32_t exclude_success)
{
  uint32_t map_success;

  int ret = ext_data_setup(client,
			   struct_layout_info, size_info,
			   struct_info, &map_success,
			   size_buf,
			   name_id, struct_id);

  if (ret == -1)
    {
      perror("ext_data_setup");
      fprintf (stderr,"Failed to setup connection: %s\n",
	       client->_last_error);
      /* Not more fatal than that we can disconnect. */
      return 0;
    }

  if (struct_map_success)
    *struct_map_success = map_success;

  /* Since we are stderr version, we check the mapping success here.
   * In order for user to not have to call another function to do the
   * check.
   */

  if (struct_info &&
      (map_success & ~exclude_success))
    {
      fprintf (stderr,
	       "Structure '%s' was not completely mapped (0x%04x).\n",
	       name_id, map_success);
      ext_data_struct_info_print_map_success(struct_info,
					     stderr,
					     EXT_DATA_ITEM_MAP_OK);
      return 0;
    }

  return 1;
}

int ext_data_nonblocking_fd_stderr(struct ext_data_client *client)
{
  int fd = ext_data_nonblocking_fd(client);

  if (fd == -1)
    {
      perror("ext_data_fetch_event");
      fprintf (stderr,"Failed to make file descriptor non-blocking: %s\n",
	       client->_last_error);
      return -1;
    }

  return fd;
}

int ext_data_next_event_stderr(struct ext_data_client *client,
			       int *struct_id)
{
  int ret = ext_data_next_event(client, struct_id);

  if (ret == 0)
    {
      fprintf (stderr,"End from server.\n");
      return 0; /* Out of data. */
    }

  if (ret == -1)
    {
      if (errno == EAGAIN)
	return -1;

      perror("ext_data_next_event");
      fprintf (stderr,"Failed to query next event: %s\n",
	       client->_last_error);
      /* Not more fatal than that we can disconnect. */
      return 0;
    }

  return 1;
}

int ext_data_fetch_event_stderr(struct ext_data_client *client,
				void *buf,size_t size,
				int struct_id)
{
  int ret = ext_data_fetch_event(client,buf,size,struct_id);

  if (ret == 0)
    {
      fprintf (stderr,"End from server.\n");
      return 0; /* Out of data. */
    }

  if (ret == -1)
    {
      if (errno == EAGAIN)
	return -1;

      perror("ext_data_fetch_event");
      fprintf (stderr,"Failed to fetch event: %s\n",
	       client->_last_error);
      /* Not more fatal than that we can disconnect. */
      return 0;
    }

  return 1;
}

int ext_data_get_raw_data_stderr(struct ext_data_client *client,
				 const void **raw,ssize_t *raw_words)
{
  int ret = ext_data_get_raw_data(client,raw,raw_words);

  if (ret != 0)
    {
      perror("ext_data_get_raw_data");
      fprintf (stderr,"Failed to get raw data: %s\n",
	       client->_last_error);
      return 0;
    }

  return 1;
}

void ext_data_close_stderr(struct ext_data_client *client)
{
  int ret = ext_data_close(client);

  if (ret != 0)
    {
      perror("ext_data_close");
      fprintf (stderr,"Errors reported during disconnect: %s\n",
	       client->_last_error);
      return;
    }

  fprintf (stderr,"Disconnected from server.\n");
}

#endif
