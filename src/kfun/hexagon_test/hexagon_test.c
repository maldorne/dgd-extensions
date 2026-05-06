/*
 * hexagon_test() kfun extension
 */

#include "lpc_ext.h"

static void hexagon_test(LPC_frame f, int nargs, LPC_value retval) {
  // LPC_value val;
  LPC_string str;
  LPC_dataspace data;
  char *p;
  unsigned int i;

  /* fetch the argument string */
  // val = lpc_frame_arg(f, nargs, 0);
  // str = lpc_string_getval(val);

  data = lpc_frame_dataspace(f);
  str = lpc_string_new(data, "Hexagon Mudlib extension test", 29);

  /* put result in return value */
  lpc_string_putval(retval, str);
}

static char hexagon_test_proto[] = {LPC_TYPE_STRING, 0};
static LPC_ext_kfun kf[1] = {"hexagon_test", hexagon_test_proto, &hexagon_test};

int lpc_ext_init(int major, int minor, const char *config) {
  lpc_ext_kfun(kf, 1);
  return 1;
}
