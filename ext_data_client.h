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

#ifndef __EXT_DATA_CLIENT_H__
#define __EXT_DATA_CLIENT_H__

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*************************************************************************/

struct ext_data_client;

struct ext_data_structure_info;

struct ext_data_structure_item;
/*************************************************************************/

/* Allocate memory to store destination structure information.
 *
 * @name            Name of structure.  If empty, the default (first)
 *                  structure is implied.
 *
 * Return value:
 *
 * Pointer to a structure, or NULL on failure.
 *
 * ENOMEM           Failure to allocate memory.
 */

struct ext_data_structure_info *ext_data_struct_info_alloc();

/*************************************************************************/

/* Deallocate structure information.
 *
 * @struct_info     Information structure.
 */

void ext_data_struct_info_free(struct ext_data_structure_info *struct_info);

/*************************************************************************/

#define EXT_DATA_ITEM_FLAGS_OPTIONAL  0x01

/* Add one item of structure information.
 *
 * @struct_info     Information structure.
 * @offset          Location of the item in the structure being described,
 *                  in bytes (use offsetof()).
 * @size            Size (in bytes) of the item (use sizeof()).
 *                  When array, size of the entire array.
 * @type            Type of the item, one of EXT_DATA_ITEM_TYPE_...
 * @prename         First part of name (when several similar structure
 *                  items exist.  Set to NULL or "" when unused.
 *                  The actual name will be a concatenation of prename,
 *                  preindex(as a string) and name.
 * @preindex        Index part of prename, set to -1 when unused.
 * @name            Name of the item.
 * @ctrl_name       Name of the controlling item (for arrays),
 *                  otherwise NULL or "".
 * @limit           Maximum value.  Needed for items that are controlling
 *                  variables.  Set to -1 when unused.
 *                  Note: fixed array size is handled by @size above.
 * @flags           Mark that an item is optional. Failure to map the item
 *                  will be reported differently (..._MAP_OPT_NOT_FOUND).
 *
 * Return value:
 *
 *  0  success.
 * -1  failure.  See errno.
 *
 * ENOMEM           Failure to allocate memory.
 * EFAULT           @struct_info is NULL.
 * EINVAL           Information inconsistency - possibly collision
 *                  with previously declared items.
 */

int ext_data_struct_info_item(struct ext_data_structure_info *struct_info,
			      size_t offset, size_t size,
			      int type,
			      const char *prename, int preindex,
			      const char *name, const char *ctrl_name,
			      int limit_max, uint32_t flags);

/*************************************************************************/

#define EXT_DATA_ITEM_TYPE_INT32     0x01
#define EXT_DATA_ITEM_TYPE_UINT32    0x02
#define EXT_DATA_ITEM_TYPE_FLOAT32   0x03
#define EXT_DATA_ITEM_TYPE_UINT64HI  0x04
#define EXT_DATA_ITEM_TYPE_UINT64LO  0x05
#define EXT_DATA_ITEM_TYPE_MASK      0x07 /* mask of previous items */
#define EXT_DATA_ITEM_HAS_LIMIT      0x08 /* flag */

/* The following convenience macros are used by auto-generated code.
 * Also suggested to be used directly???
 *
 * @ok              Return variable, set to 0 on failure.
 * @struct_info     Information structure.
 * @offset          Offset of structure from base pointer (buf) of
 *                  ext_data_fetch_event().  Only set to non-0 if
 *                  @struct_t is a substructure of buf.
 * @struct_t        The enclosing structure type.
 * @printerr        If set non-zero, use _stderr version of code
 *                  to print errors.
 * @item            Name of structure item (in struct_t).
 * @type            Type of structure item, one of INT32, UINT32, FLOAT32.
 * @name            Name of item to use in the sent data stream.
 * @ctrl            Name of the controlling item (for arrays),
 *                  otherwise NULL or "".
 * @limit           Maximum value.  Needed for items that are controlling
 *                  variables.  Set to -1 when unused.
 *                  Note: fixed array size is handled by @size above.
 * @flags           Mark that an item is optional. Failure to map the item
 *                  will be reported differently (..._MAP_OPT_NOT_FOUND).
 *
 * Failure or success is reported in @ok.
 *
 */

#define EXT_STR_ITEM_INFO2_ALL(ok,struct_info,offset,struct_t,printerr,	\
			       item,type,name,ctrl,limit_max,flags)	\
  if (!printerr) {							\
    ok &= (ext_data_struct_info_item(struct_info,			\
				     offsetof (struct_t, item)+offset,	\
				     sizeof (((struct_t *) 0)->item),	\
				     EXT_DATA_ITEM_TYPE_##type,		\
				     "", -1,				\
				     name, ctrl, limit_max, flags) == 0); \
  } else {								\
    ok &= ext_data_struct_info_item_stderr(struct_info,			\
					   offsetof (struct_t, item)+offset, \
					   sizeof (((struct_t *) 0)->item), \
					   EXT_DATA_ITEM_TYPE_##type,	\
					   "", -1,			\
					   name, ctrl, limit_max, flags); \
  }

#define EXT_STR_ITEM_INFO2(ok,struct_info,offset,struct_t,printerr,	\
			   item,type,name,flags)			\
  EXT_STR_ITEM_INFO2_ALL(ok,struct_info,offset,struct_t,printerr,	\
			 item,type,name,"",-1,flags)

#define EXT_STR_ITEM_INFO2_ZZP(ok,struct_info,offset,struct_t,printerr,	\
			       item,type,name,ctrl,flags)		\
  EXT_STR_ITEM_INFO2_ALL(ok,struct_info,offset,struct_t,printerr,	\
			 item,type,name,ctrl,-1,flags)

#define EXT_STR_ITEM_INFO2_LIM(ok,struct_info,offset,struct_t,printerr,	\
			       item,type,name,limit_max,flags)		\
  EXT_STR_ITEM_INFO2_ALL(ok,struct_info,offset,struct_t,printerr,	\
			 item,type,name,"",limit_max,flags)

/* These are to support old generated code which did not have the
 * flags parameter.
 */

#define EXT_STR_ITEM_INFO(ok,struct_info,offset,struct_t,printerr,	\
			  item,type,name)				\
  EXT_STR_ITEM_INFO2_ALL(ok,struct_info,offset,struct_t,printerr,	\
			 item,type,name,"",-1,0)

#define EXT_STR_ITEM_INFO_ZZP(ok,struct_info,offset,struct_t,printerr,	\
			      item,type,name,ctrl)			\
  EXT_STR_ITEM_INFO2_ALL(ok,struct_info,offset,struct_t,printerr,	\
			 item,type,name,ctrl,-1,0)

#define EXT_STR_ITEM_INFO_LIM(ok,struct_info,offset,struct_t,printerr,	\
			      item,type,name,limit_max)			\
  EXT_STR_ITEM_INFO2_ALL(ok,struct_info,offset,struct_t,printerr,	\
			 item,type,name,"",limit_max,0)

/*************************************************************************/

/* Return a pointer to a (static) string with a more descriptive error
 * message.
 */

const char *
ext_data_struct_info_last_error(struct ext_data_structure_info *struct_info);

/*************************************************************************/

#define EXT_DATA_ITEM_MAP_MATCH          0x0001u /* Perfect match. */
#define EXT_DATA_ITEM_MAP_NO_DEST        0x0002u /* Left-over server item. */
#define EXT_DATA_ITEM_MAP_NOT_FOUND      0x0004u /* Item not found. */
#define EXT_DATA_ITEM_MAP_TYPE_MISMATCH  0x0008u /* Server item has different
						    type. */
#define EXT_DATA_ITEM_MAP_CTRL_MISMATCH  0x0010u /* Server item has different
						    controlling item. */
#define EXT_DATA_ITEM_MAP_ARRAY_FEWER    0x0020u /* Array, client shorter. */
#define EXT_DATA_ITEM_MAP_ARRAY_MORE     0x0040u /* Array, client longer. */
#define EXT_DATA_ITEM_MAP_NOT_DONE       0x0080u /* Never reached mapping. */
#define EXT_DATA_ITEM_MAP_OPT_NOT_FOUND  0x0100u /* Optional item not found. */

/* This mask identifies mappings which loose no data. */
#define EXT_DATA_ITEM_MAP_OK            (EXT_DATA_ITEM_MAP_MATCH | \
					 EXT_DATA_ITEM_MAP_ARRAY_MORE | \
					 EXT_DATA_ITEM_MAP_OPT_NOT_FOUND)
/* This mask identifies mappings which may drop data from server,
 * but all client items had sources.
 */
#define EXT_DATA_ITEM_MAP_OK_NO_DEST    (EXT_DATA_ITEM_MAP_OK | \
					 EXT_DATA_ITEM_MAP_NO_DEST)

/* Return information on how the structure members were mapped from
 * the data structure given by the server after ext_data_setup().  The
 * result is given in the return arguments.  @map_success is one of
 * the constants EXT_DATA_ITEM_MAP_... above.
 *
 * First call this function with @restart = 1, and as long as it
 * returns success, with @restart = 0.
 *
 * Note that after all client items, information is also reported
 * about items in the server structure that have not been mapped,
 * i.e. ..._NO_DEST.  For server items, @offset is returned as -1.
 *
 * Do not call this function after the ext_data_client connection has
 * been closed.
 *
 * Note the convenience function ext_data_struct_info_print_map_success()
 * below!
 *
 * @struct_info     Information structure.
 * @restart         Set to 1 on first call, then 0.
 * @var_name        Name of item (result).  Note: pointer to const char *
 *                  pointer, will receive a pointer to the name string.
 * @offset          Offset of item (result).  (-1 for server items.)
 * @map_success     Matching outcome (result).
 *
 * Return value:
 *
 *  1  success (item returned).
 *  0  end of data (index outside array).
 * -1  failure.  See errno.
 *
 * Possible errors:
 *
 * EFAULT           @struct_info is NULL.
 * EINVAL           Structure has not been mapped, i.e. was not used
 *                  with ext_data_setup().
 */

int
ext_data_struct_info_map_success(struct ext_data_structure_info *struct_info,
				 int restart,
				 const char **var_name,
				 uint32_t *offset,
				 uint32_t *map_success);

/*************************************************************************/

/* Print a table of members and their mapping success, after call to
 * ext_data_setup().
 *
 * Do not call this function after the ext_data_client connection has
 * been closed.
 *
 * @struct_info     Information structure.
 * @stream          FILE pointer for output.
 * @exclude_success Flags to not include in listing.
 *                  (EXT_DATA_ITEM_MAP_OK only prints causing data loss).
 *
 * Return value:
 *
 *  1  success (item returned).
 * -1  failure.  See errno.
 *
 * Possible errors:
 *
 * EFAULT           @struct_info is NULL.
 * EINVAL           Structure has not been mapped, i.e. was not used
 *                  with ext_data_setup().
 */

int
ext_data_struct_info_print_map_success(struct ext_data_structure_info *
				       /* */ struct_info,
				       FILE *stream,
				       uint32_t exclude_success);

/*************************************************************************/

/* Constants for time-stitching irregularities. */

/* There was another (unmerged) event within the timestamp merge window. */
#define EXT_FILE_MERGE_NEXT_SRCID_WITHIN_WINDOW  0x01
/* There was a previous (unmerged) event within the timestamp merge window. */
/* The previous event would have EXT_FILE_MERGE_NEXT_SRCID_WITHIN_WINDOW set.*/
#define EXT_FILE_MERGE_PREV_SRCID_WITHIN_WINDOW  0x02
/* Several sources had non-zero value for a static integer value. */
#define EXT_FILE_MERGE_MULTIPLE_IVALUE           0x04
/* Several sources had non-zero value for a static float value. */
#define EXT_FILE_MERGE_MULTIPLE_FVALUE           0x08
/* Too many items for array - overflow. */
#define EXT_FILE_MERGE_ARRAY_OVERFLOW            0x10
/* Too many items for multi-item indexed array - overflow. */
#define EXT_FILE_MERGE_ARRAY_MINDEX_OVERFLOW     0x20

/*************************************************************************/

/* Connect to a TCP external data client.
 *
 * @server          Either a HOSTNAME or a HOSTNAME:PORT.
 *                  Alternatively "-" may be specified to use stdin
 *                  (STDIN_FILENO).
 *
 * Return value:
 *
 * Pointer to a context structure (use when calling other functions).
 * NULL on failure, in which case errno describes the error.
 *
 * In addition to various system (socket etc) errors, errno:
 *
 * EPROTO           Protocol error, version mismatch?
 * ENOMEM           Failure to allocate memory.
 * ETIMEDOUT        Timeout while trying to get data port number.
 *
 * EHOSTDOWN        Hostname not found (not really correct).
 */

struct ext_data_client *ext_data_connect(const char *server);

/*************************************************************************/

/* Create a client context to read data from a file descriptor (e.g. a
 * pipe).  Note: while ext_data_close() need to be called to free the
 * client context, it will not close the file descriptor.
 *
 * Use fileno(3) to attach to a FILE* stream.
 *
 * Return value:
 *
 * Pointer to a context structure (use when calling other functions).
 * NULL on failure, in which case errno describes the error:
 *
 * ENOMEM           Failure to allocate memory.
 */

struct ext_data_client *ext_data_from_fd(int fd);

/*************************************************************************/

/* Create a client context to write events using stdout (STDOUT_FILENO).
 * Then call the setup function to describe the data structure.
 *
 * Return value:
 *
 * Pointer to a context structure (use when calling other functions).
 * NULL on failure, in which case errno describes the error:
 *
 * ENOMEM           Failure to allocate memory.
 */

struct ext_data_client *ext_data_open_out();

/*************************************************************************/

/* Consume the header messages and verify that the structure to
 * be filled is correct - alternatively map as many members as is
 * possible.
 *
 * @client              Connection context structure.
 * @name_id             Name of structure.  If empty, the default (first)
 *                      structure is implied.
 * @struct_id           Returns unique integer to identify the structure,
 *                      May be null if name is empty.
 * @struct_layout_info  Pointer to an instance of the structure
 *                      layout information generated together with
 *                      the structure to be filled.
 *                      Can be NULL, if @struct_info is given.
 * @size_info           Size of the struct_layout_info structure.
 *                      Use sizeof(struct).
 * @struct_info         Detailed structure member information.
 *                      Allows mapping of items when sending structure
 *                      is different from structure to be filled.
 * @struct_map_success  Returns overall mapping success of @struct_info.
 *                      In order to not have silent data loss, it is
 *                      suggested to only continue if
 *                      (struct_map_success & ~EXT_DATA_ITEM_MAP_OK).
 *                      Or with EXT_DATA_ITEM_MAP_OK_NO_DEST to just
 *                      ensure that all destination items got data.
 *                      ext_data_struct_info_print_map_success() can be
 *                      used to print a report of mapping issues.
 *                      Can be NULL (no mapping possible).
 * @size_buf            Size of the structure to be filled.
 *                      Use sizeof(struct).
 *
 * Return value:
 *
 *  0  success.
 * -1  failure.  See errno.
 *
 * In addition to various system (socket etc) errors, errno:
 *
 * EINVAL           @struct_layout_info is wrong.
 * ENOMSG           @name_id does not exist in server structure list.
 * EBUSY            Structure has already been mapped.
 * EPROTO           Protocol error, version mismatch?
 * EBADMSG          Internal protocol fault.  Bug?
 * ENOMEM           Failure to allocate memory.
 * EFAULT           @client is NULL.
 *                  @struct_layout_info and @struct_info is NULL.
 */

int ext_data_setup(struct ext_data_client *client,
		   const void *struct_layout_info,size_t size_info,
		   struct ext_data_structure_info *struct_info,
		   uint32_t *struct_map_success,
		   size_t size_buf,
		   const char *name_id, int *struct_id);

/*************************************************************************/

/* Make the file descriptor non-blocking, such that clients can fetch
 * events in some loop that also controls other stuff, e.g. an interactive
 * program.  The file descriptor is returned.
 *
 * After this call, ext_data_fetch_event() may return failure (-1) with
 * errno set to EAGAIN.
 *
 * Note that if the user fails to check if any data is available
 * before calling ext_data_fetch_event() again, using e.g. select() or
 * poll(), then the program will burn CPU uselessly in a busy-wait
 * loop.
 *
 * Note that for the time being, this function must be called after
 * ext_data_setup() (which currently requires blocking access).
 *
 * Return value:
 *  n  file descriptor.
 * -1  failure, see errno.
 *
 * Error codes on failure come from fcntl().
 */

int ext_data_nonblocking_fd(struct ext_data_client *client);

/*************************************************************************/

/* Tell which structure will be returned next.
 * Structures not registered with ext_data_setup will be ignored.
 *
 * @client          Connection context structure.
 * @name_id         Pointer to return id of next event.
 *
 * If called multiple times, then events are discarded.  Can be used
 * to skip unwanted structures.
 *
 * Return value:
 *
 *  1  success (one event can be fetched).
 *  0  end-of-data.
 * -1  failure.  See errno.
 *
 * In addition to various system (socket read) errors, errno:
 *
 * EFAULT           @client is NULL.
 * EBADMSG          Data offset outside structure.
 *                  Malformed message.  Bug?
 * EPROTO           Unexpected message.  Bug?
 * EFAULT           @client is NULL.
 * EAGAIN           No further data at this moment (after using
 *                  ext_data_nonblocking_fd()).
 */

int ext_data_next_event(struct ext_data_client *client,
			int *struct_id);

/*************************************************************************/

/* Fetch one event from an open connection (buffered) into a
 * user-provided buffer.
 *
 * @client          Connection context structure.
 * @buf             Pointer to the data structure.
 *                  A header file with the appropriate structure
 *                  declaration can be generated by struct_writer.
 * @size            Size of the structure.  Use sizeof(struct).
 * @struct_id       Key of next structure to retrieve, 0 if name_id = "".
 *
 * Note that not necessarily all structure elements are filled -
 * zero-suppression.  It is up to the user to obey the conventions
 * imposed by the data transmitted.  Calling ext_data_rand_fill() to
 * fill the event buffer before each event can be used as a crude tool
 * to detect mis-use of data.
 *
 * Return value:
 *
 *  1  success (one event fetched).
 *  0  end-of-data.
 * -1  failure.  See errno.
 *
 * In addition to various system (socket read) errors, errno:
 *
 * EINVAL           @size is wrong.
 * EINVAL           @struct_id is wrong.
 * EBADMSG          Data offset outside structure.
 *                  Malformed message.  Bug?
 * EPROTO           Unexpected message.  Bug?
 * EFAULT           @client is NULL.
 * EAGAIN           No further data at this moment (after using
 *                  ext_data_nonblocking_fd()).
 */

int ext_data_fetch_event(struct ext_data_client *client,
			 void *buf,size_t size
#if !STRUCT_WRITER
			 ,int struct_id
#endif
#if STRUCT_WRITER
			 ,struct external_writer_buf_header **header_in
			 ,uint32_t *length_in
#endif
			 );

/*************************************************************************/

/* Get the ancillary raw data (if any) associated with the last
 * fetched event.
 *
 * @client          Connection context structure.
 * @raw             Will receive a pointer to a range of raw data,
 *                  if such is available within the event, otherwise NULL.
 *                  The pointer is valid until the next event is fetched.
 *                  The pointer is guaranteed to be 32-bit aligned.
 * @raw_words       Amount of data in the raw range (32-bit words).
 *
 * Note that the raw data will be delivered byte-swapped as 32-bit items.
 * (This is not really a sane handling, but matches the usual treatment
 * of .lmd file data, for which it is currently used...)
 *
 * Return value:
 *
 *  0  success (pointers set).
 * -1  failure.  See errno.
 *
 * Possible errors:
 *
 * EINVAL           @raw or @raw_words is NULL.
 * EFAULT           @client is NULL.
 *
 */

int ext_data_get_raw_data(struct ext_data_client *client,
			  const void **raw,ssize_t *raw_words);

/*************************************************************************/

/* Clear the user-provided buffer with zeros and NaNs.
 * Useful when writing events.
 *
 * @client          Connection context structure.
 * @buf             Pointer to the data structure.
 * @size            Size of the structure.  Use sizeof(struct).
 * @clear_zzp_lists Clear contents of variable sized (zero-suppressed)
 *                  lists.  Should be 0 for performance, see below.
 * @struct_id       Key of structure to clear, 0 if name_id = "".
 *
 * The buffer is cleared with zeros and NaNs (for int and float
 * variables respectively) as specified by the pack list of the data
 * connection.
 *
 * Return value:
 *  0  success.
 * -1  failure.  See errno:
 *
 * EINVAL           @size is wrong.
 * EINVAL           @struct_id is wrong.
 * EFAULT           @client is NULL.
 */

int ext_data_clear_event(struct ext_data_client *client,
			 void *buf,size_t size,int clear_zzp_lists,
			 int struct_id);

/*************************************************************************/

/* Clear the items in the user-provided buffer related to one particular
 * controlling item.
 * Useful when writing events.
 *
 * @client          Connection context structure.
 * @buf             Pointer to the data structure.
 * @item            Controlling item.
 * @struct_id       Key of structure to clear, 0 if name_id = "".
 *
 * With zero-suppression, it is important that all entries of
 * variable-sized arrays up to the index specified by the controlling
 * item are set/cleared for each event.  As it often is prohibitively
 * expensive to clear all entries (i.e. also unused ones), the
 * clear_zzp_lists argument is generally set to 0 when calling
 * ext_data_clear_event (see above).
 *
 * Either the user code must always set all affected items, or it may,
 * after setting a controlling item (but before any controlled items),
 * call this function to perform the necessary zero/NaN-ing.
 *
 * Note: this function is expected to be called frequently and does
 * _not_ validate the input parameters.  (For the benefit of the
 * user-code then also having no return value to care about :-) )
 * Take care!
 */

void ext_data_clear_zzp_lists(struct ext_data_client *client,
                              void *buf,void *item,
			      int struct_id);

/*************************************************************************/

/* Pack and write one one event from a user-provided buffer to an open
 * connection (buffered).
 *
 * @client          Connection context structure.
 * @buf             Pointer to the data structure.
 *                  A header file with the appropriate structure
 *                  declaration can be generated by struct_writer.
 * @size            Size of the structure.  Use sizeof(struct).
 *
 * Note that not necessarily all structure elements are used -
 * zero-suppression.  It is up to the user to obey the conventions
 * imposed by the data transmitted.  Use ext_data_clear_event() to
 * wipe the user buffer with zeros and NaNs (for int and float
 * variables respectively) before filling each event.
 *
 * Calling ext_data_rand_fill() to fill the event buffer before each
 * event can be used as a crude tool to detect mis-use of data (at the
 * receiving end).
 *
 * Return value:
 *
 *  size       success (one event written (buffered)).
 *  0..size-1  data malformed (zero-suppression control item has too
 *             large value), return value gives offset, event is NOT
 *             written.
 * -1          failure.  See errno.
 *
 * In addition to various system (socket write) errors, errno:
 *
 * EINVAL           @size is wrong.
 * EBADMSG          Data offset outside structure.
 *                  Malformed message.  Bug?
 * EPROTO           Unexpected message.  Bug?
 * EFAULT           @client is NULL.
 */

int ext_data_write_event(struct ext_data_client *client,
			 void *buf,size_t size);

/*************************************************************************/

/* Flush the write buffer of events.
 *
 * @client          Connection context structure.
 *
 * Calling this function is not necessary.  Only use it if the program
 * knows it has to wait for external input and wants to send the
 * previous events along before more is produced.
 *
 * Return value:
 *
 *  0  success.
 * -1  failure.  See errno.
 *
 * In addition to various system (socket close) errors, errno:
 *
 * EFAULT           @client is NULL, or not properly set up.
 */

int ext_data_flush_buffer(struct ext_data_client *client);

/*************************************************************************/

/* Close a client connection, and free the context structure.
 *
 * @client          Connection context structure.
 *
 * Return value:
 *
 *  0  success.
 * -1  failure.  See errno.
 *
 * In addition to various system (socket close) errors, errno:
 *
 * EFAULT           @client is NULL, or not properly set up.
 */

int ext_data_close(struct ext_data_client *client);

/*************************************************************************/

/* Fill a buffer with some sort of (badly, i.e. not so) random data.
 *
 * @buf             Pointer to the data structure.
 * @size            Size of the structure.  Use sizeof(struct).
 *
 * Useful to verify program logics and integrity of zero-suppressed
 * data.  Pseudo-random number sequence seed fixed.
 *
 * Failure not thinkable - providing junk always possible.
 */

void ext_data_rand_fill(void *buf,size_t size);

/*************************************************************************/

/* Return a pointer to a (static) string with a more descriptive error
 * message.
 */

const char *ext_data_last_error(struct ext_data_client *client);

/*************************************************************************/

/* The following functions are the same as those above, except that
 * errors are caught and messages printed to stderr.
 *
 * Exception: ext_data_setup_stderr has additional argument:
 * @exclude_success  See ext_data_struct_info_map_success,
 *                   typical use: EXT_DATA_ITEM_MAP_OK.
 *
 * Returns:
 *
 * ext_data_struct_info_alloc_stderr   pointer or NULL
 * ext_data_struct_info_free           1 or 0 (failure)
 *
 * ext_data_connect_stderr        pointer or NULL
 * ext_data_setup_stderr          1 or 0
 * ext_data_nonblocking_fd_stderr fd or -1
 * ext_data_fetch_event_stderr    1 or 0 (got event, or end of data)
 *                                or -1 (for EAGAIN, with non-blocking)
 * ext_data_get_raw_data_stderr   1 or 0
 * ext_data_clear_event_stderr    1 or 0
 * ext_data_write_event_stderr    1 or 0
 * ext_data_setup_stderr          1 or 0 (failure)
 * ext_data_close_stderr          (void)
 */

struct ext_data_structure_info *ext_data_struct_info_alloc_stderr();


struct ext_data_structure_item* ext_data_struct_info_get_items(struct ext_data_structure_info * info);


int ext_data_struct_info_item_stderr(struct ext_data_structure_info *struct_info,
				     size_t offset, size_t size,
				     int type,
				     const char *prename, int preindex,
				     const char *name, const char *ctrl_name,
				     int limit_max, uint32_t flags);

struct ext_data_client *ext_data_connect_stderr(const char *server);

struct ext_data_client *ext_data_open_out_stderr();

int ext_data_setup_stderr(struct ext_data_client *client,
			  const void *struct_layout_info,size_t size_info,
			  struct ext_data_structure_info *struct_info,
			  uint32_t *struct_map_success,
			  size_t size_buf,
			  const char *name_id, int *struct_id,
			  uint32_t exclude_success);

/* In case using code needs to compile also with earlier version. */
#define EXT_DATA_SETUP_STDERR_HAS_EXCLUDE_SUCCESS 1

int ext_data_nonblocking_fd_stderr(struct ext_data_client *client);

int ext_data_next_event_stderr(struct ext_data_client *client,
			       int *struct_id);

int ext_data_fetch_event_stderr(struct ext_data_client *client,
				void *buf,size_t size,
				int struct_id);

int ext_data_get_raw_data_stderr(struct ext_data_client *client,
				 const void **raw,ssize_t *raw_words);

int ext_data_clear_event_stderr(struct ext_data_client *client,
				void *buf,size_t size,int clear_zzp_lists,
				int struct_id);

int ext_data_write_event_stderr(struct ext_data_client *client,
				void *buf,size_t size,
				int struct_id);

int ext_data_flush_buffer_stderr(struct ext_data_client *client);

void ext_data_close_stderr(struct ext_data_client *client);

/*************************************************************************/

#ifdef __cplusplus
}
#endif

#endif/*__EXT_DATA_CLIENT_H__*/
