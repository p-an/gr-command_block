/* -*- c++ -*- */
/* 
 * Copyright 2016 <+YOU OR YOUR COMPANY+>.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "command_impl.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

#include <inttypes.h>

//#define DBG(f, x...) {fprintf(stdout,"[%s() %d %p]: " f, __func__, __LINE__ ,this, ##x); fflush(stdout);}
//#define DBG(f, x...) {fprintf(stderr,"[%s() %d]: " f, __func__, __LINE__ ,##x); fflush(stderr);}
//#define DBG(f, x...) {fprintf(stderr,"[%s() %d %p]: " f, __func__, __LINE__ ,this, ##x); fflush(stderr);}
#define DBG(f, x...)

namespace gr {
  namespace command_block {

    enum { TYPE_IN=1, TYPE_OUT=2 };
    enum { BLOCKING_NONE=0, BLOCKING_IN=1, BLOCKING_OUT=2 };//BLOCKING_both = BLOCKING_IN | BLOCKING_OUT == 3

    command::sptr
    command::make(size_t in_item_size, size_t out_item_size, const char* cmd, double io_ratio, int blocking)
    {
      return gnuradio::get_initial_sptr
        (new command_impl(in_item_size, out_item_size, cmd, io_ratio, blocking));
    }

    /*
     * The private constructor
     */
    command_impl::command_impl(size_t in_item_size, size_t out_item_size, const char* cmd, double io_ratio, int blocking)
      : gr::block("command",
              gr::io_signature::make(0, (in_item_size > 0)?1:0, in_item_size),
              gr::io_signature::make(0, (out_item_size > 0)?1:0, out_item_size)),
          io_ratio(io_ratio),in_item_size(in_item_size),out_item_size(out_item_size),blocking(blocking)
    {
        this->cmd = new char [strlen(cmd) + 1];
        strcpy(this->cmd,cmd);
        type = 0;
        if(in_item_size > 0)type |= TYPE_IN;
        if(out_item_size > 0)type |= TYPE_OUT;
        w_byte_corr = 0;
        r_byte_corr = 0;
        r_byte_corr_buff = new uint8_t[out_item_size];

        DBG("in %d out %d cmd \"%s\"\n",(type & TYPE_IN),(type & TYPE_OUT)>>1, cmd);
        DBG("blocking in %d out %d\n",blocking & BLOCKING_IN, (blocking & BLOCKING_OUT)>>1);
        total_consumed = 0;
        total_produced = 0;

    }

    /*
     * Our virtual destructor.
     */
    command_impl::~command_impl()
    {
      delete cmd;
    }

    bool command_impl::start(){
      int stdin_pipe[2];
      int stdout_pipe[2];

      if(pipe(stdin_pipe) != 0){
        throw errno_exception("pipe()", errno);
      }

      if(pipe(stdout_pipe) != 0){
        throw errno_exception("pipe()", errno);
      }

      pid = fork();
      if (pid == -1) {
        return false;
      }
      else if (pid == 0) {

        if(type & TYPE_IN){
          dup2(stdin_pipe[0], STDIN_FILENO);
          close(stdin_pipe[0]);
          close(stdin_pipe[1]);
        }

        if(type & TYPE_OUT){
          dup2(stdout_pipe[1], STDOUT_FILENO);
          close(stdout_pipe[1]);
          close(stdout_pipe[0]);
        }
        DBG("'%s'\n",cmd)
        execl("/bin/sh", "sh", "-c", cmd, NULL);

        exit(EXIT_FAILURE);
      }
      else {

        if(type & TYPE_IN){

          close(stdin_pipe[0]);
          if(blocking & BLOCKING_IN)unset_fd_flags(stdin_pipe[1], O_NONBLOCK);
          else set_fd_flags(stdin_pipe[1], O_NONBLOCK);
          fcntl(stdin_pipe[1], F_SETFD, FD_CLOEXEC);

          cmd_in_fd = stdin_pipe[1];
        }

        if(type & TYPE_OUT){
          if(blocking & BLOCKING_OUT)unset_fd_flags(stdout_pipe[0], O_NONBLOCK);
          else set_fd_flags(stdout_pipe[0], O_NONBLOCK);
          fcntl(stdout_pipe[0], F_SETFD, FD_CLOEXEC);
          close(stdout_pipe[1]);

          cmd_out_fd = stdout_pipe[0];
        }
      }

      return true;
    }   

    bool command_impl::stop(){
      pid_t rc;
      char buf[PIPE_BUF];
      ssize_t sz;
      int i;
      int pstat;

      DBG("cmd %s out %" PRIu64 " in %" PRIu64 " out/in %f\n",cmd,total_produced,total_consumed,((float)total_produced)/total_consumed);

      kill(pid,SIGTERM);
      do {
        rc = waitpid(pid, &pstat, 1);
      } while (rc == -1 && errno == EINTR);

      if (rc == -1) {
        return true;
      }

      if (WIFEXITED(pstat))
        std::cerr << "Process \"" << cmd << "\" exited with code " << WEXITSTATUS(pstat) << std::endl;
      else
        std::cerr << "\"" << cmd << "\" exited" << std::endl;

      return true;
    }

    void
    command_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
    {
        int min = 1;
        ninput_items_required[0] = noutput_items / io_ratio;
        if(ninput_items_required[0] < min)ninput_items_required[0]=min;
    }


    int
    command_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      int produced = 0;
      int consumed = 0;

      if(type & TYPE_IN){
        uint8_t *in = (uint8_t *) input_items[0];
        size_t n_items_in = ninput_items[0];

        ssize_t c = write(cmd_in_fd,in + w_byte_corr ,in_item_size*n_items_in - w_byte_corr);
        if(c < 0){
          if(errno == EAGAIN || errno == EWOULDBLOCK){
            //DBG("EAGAIN\n");
          }else{
            throw errno_exception("write()", errno);
          }
        }else{
          c += w_byte_corr;
          w_byte_corr = c % in_item_size;
          consumed = c / in_item_size;
          consume_each(consumed);
        }
      }

      if(type & TYPE_OUT){
        uint8_t * out = (uint8_t *) output_items[0];
        size_t n_items_out = noutput_items;
        memcpy(out,r_byte_corr_buff,r_byte_corr);
        if(r_byte_corr>0)n_items_out--;
        ssize_t c = read(cmd_out_fd,out + r_byte_corr ,n_items_out*out_item_size);
        if(c < 0){
          if(errno == EAGAIN || errno == EWOULDBLOCK){
            //DBG("EAGAIN\n");
          }else{
            throw errno_exception("read()", errno);
          }
        }else{
          c += r_byte_corr;
          r_byte_corr = c % out_item_size;
          if(r_byte_corr != 0){
            memcpy(r_byte_corr_buff,out + (c-r_byte_corr),r_byte_corr);
          }
          produced = c / out_item_size;
          if(c == 0)produced = -1; //EOF
        }
      }

      total_produced += produced * out_item_size;
      total_consumed += consumed * in_item_size;
      //if(produced != 0)DBG("out %" PRIu64 " in %" PRIu64 " out/in %f\n",total_produced,total_consumed,((float)total_produced)/total_consumed);

      // Tell runtime system how many output items we produced.
      return produced;
    }

    void command_impl::set_fd_flags(int fd, int flags)
    {
      int rc;

      rc = fcntl(fd, F_GETFL);
      if (rc == -1) {
        throw errno_exception("fcntl()", errno);
      }

      rc = fcntl(fd, F_SETFL, rc | flags);
      if (rc == -1) {
        throw errno_exception("fcntl()", errno);
      }
    }

    void command_impl::unset_fd_flags(int fd, int flags)
    {
      int rc;

      rc = fcntl(fd, F_GETFL);
      if (rc == -1) {
        throw errno_exception("fcntl()", errno);
      }

      rc = fcntl(fd, F_SETFL, rc & ~flags);
      if (rc == -1) {
        throw errno_exception("fcntl()",errno);
      }
    }

    //FIXME std::system_error may be better for this
    std::runtime_error command_impl::errno_exception(const char * msg, int err){
      std::stringstream s;
      s << msg << " " << strerror(err) << std::endl;
      return std::runtime_error(s.str());
    }

#ifdef test_byte_corr
    ssize_t command_impl::err_write(int fd, const void *buf, size_t count){
      int e = 0;
      e = rand() % 10;
      if(e>(in_item_size - 1))e=0;
      if(e > 0 && count > e){
        count -= e;
        DBG("%d\n",e);
      }
      return write(fd,buf,count);
    }

    ssize_t command_impl::err_read(int fd, void *buf, size_t count){
      int e = 0;
      e = rand() % 10;

      if(e>(out_item_size - 1))e=0;
      if(count > 0 && e > 0 && count > e){
        count -= e;
        DBG("%d\n",e);
      }
      ssize_t rc = read(fd,buf,count);
      return rc;
    }
#endif

  } /* namespace command_block */
} /* namespace gr */

