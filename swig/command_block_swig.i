/* -*- c++ -*- */

#define COMMAND_BLOCK_API

%include "gnuradio.i"			// the common stuff

//load generated python docstrings
%include "command_block_swig_doc.i"

%{
#include "command_block/command.h"
%}


%include "command_block/command.h"
GR_SWIG_BLOCK_MAGIC2(command_block, command);
