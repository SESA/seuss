//          Copyright Boston University SESA Group 2013 - 2016.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
#ifndef SEUSS_H
#define SEUSS_H

namespace seuss {

/** Statics for the runtime of a function */
struct ExecStats {
  size_t run_time;
  size_t init_time;
  bool status; // success=0 failed=1
};

/** Function activation record */
struct InvocationStats {
  size_t transaction_id; // unique id for invocation
  size_t function_id;  
  size_t args_size;
  char activation_id[34] = {0};
  ExecStats exec = {0}; // zero fill
};

struct Invocation {
  InvocationStats info;
  std::string code;
  std::string args;
};

} // end seuss
#endif
