//#include <ext_data_clnt.hh>

#define EXT_DATA_CLIENT_INTERNALS
#include "ext_data_client.h"
#include <string.h>

//#include <ext_data_struct_info.hh>

#include <errno.h>
int main()
{

   uint32_t map_success = 0;

   FILE* input=fopen("events.struct", "r");
   ext_data_client* cl=ext_data_from_fd(fileno(input));
   int res{};
#define CHECK(name, exp) if (res!=exp) \
   {\
    printf("%s failed with %d, error %s. errno=%d:%s\n", name, \
       res, ext_data_last_error(cl), errno, strerror(errno));\
       	return 1; }

    ext_data_structure_info* info = ext_data_struct_info_alloc();
    res=ext_data_setup(cl, NULL, 0, info, &map_success, 0, "", nullptr); CHECK("setup", 0);

     //res|=ext_data_setup_messages(cl);

   struct ext_data_structure_item* items=ext_data_struct_info_get_items(info);
   int tot=0;
   int offs=-1;

   const char* target="TIMESTAMP_LOS_ID";
   while(items)
   {
      if (items->_var_name[0]!='N')
	printf("%s 0x%x, %d %s\n", items->_var_name, items->_var_type, items->_length, items->_var_ctrl_name);
      tot+=items->_length;
      if (!strcmp(target, items->_var_name))
      {
	offs= items->_offset;
	printf("found offset of %d for %s, size=%d\n", offs, target, items->_length);
      }
      items=items->_next_off_item;
   }
   res=(offs==-1) ; CHECK("target searching", 0);
   //ext_data_struct_info_print_map_success(info, stdout, 0);
   char* buf=(char*)malloc(tot);
   printf("Allocated %d bytes\n", tot);
   uint32_t& targetval=*reinterpret_cast<uint32_t*>(buf+offs);
   printf("offset is %d\n", offs);
   for (int i=0; i<50; i++)
   {
      memset(buf, 0xff, tot);
      res=ext_data_fetch_event(cl, buf, tot, 0); CHECK("fetch", 1);
      printf("%s=%x\n", target,  targetval);
   }
   printf("status=0x%x\n", res);
   ext_data_close(cl);
   ext_data_struct_info_free(info);

   return 0;
}
