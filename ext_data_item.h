// Note: this file exposes a part of the internals of libext_data_clnt
// Normally, users should not include it. 

#ifndef __EXT_DATA_ITEM_H___
#define __EXT_DATA_ITEM_H___
struct ext_data_structure_item
{
  uint32_t    _offset;       /* Not used within STRUCT_WRITER itself. */
  uint32_t    _length;       /* not needed, info only */
  const char *_block;        /* not needed, info only */

  const char *_var_name;
  uint32_t    _var_array_len;
  const char *_var_ctrl_name;
  uint32_t    _var_type;
  uint32_t    _limit_min;
  uint32_t    _limit_max;
  uint32_t    _flags;

  uint32_t    _map_success;

#if STRUCT_WRITER
  uint32_t    _ctrl_offset;
#endif
  /* Used for remapping. */
  struct ext_data_structure_item *_ctrl_item;
  struct ext_data_structure_item *_next_off_item;
  /* Temporary used while creating remap list. */
  struct ext_data_structure_item *_match_item;
  struct ext_data_structure_item *_child_item;
};

#endif

