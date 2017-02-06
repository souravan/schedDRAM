#include <stdio.h>
#include "utlist.h"
#include "utils.h"
#include "memory_controller.h"
#include "params.h"


/* A data structure to see if a bank is a candidate for precharge. */

int priority[MAX_NUM_CHANNELS][16];
int Highest_priority;
long long int numAutoPrecharge;
int recent_colacc[MAX_NUM_CHANNELS][MAX_NUM_RANKS][MAX_NUM_BANKS];

void init_scheduler_vars()
{
  for(int channel=0;channel<MAX_NUM_CHANNELS;channel++)
  {
    for(int core=0;core<NUMCORES;core++)
    {
      priority[channel][core]=0;
      for (int bank=0; bank<MAX_NUM_BANKS; bank++)
      {
        recent_colacc[channel][core][bank] = 0;
      }
    }
  }
  Highest_priority=max(2*NUMCORES,12);
  numAutoPrecharge=0;
}

// write queue high water mark; begin draining writes if write queue exceeds this value
#define HI_WM 40

// end write queue drain once write queue has this many writes in it
#define LO_WM 20

// 1 means we are in write-drain mode for that channel
int drain_writes[MAX_NUM_CHANNELS];

/* Each cycle it is possible to issue a valid command from the read or write queues
   OR
   a valid precharge command to any bank (issue_precharge_command())
   OR
   a valid precharge_all bank command to a rank (issue_all_bank_precharge_command())
   OR
   a power_down command (issue_powerdown_command()), programmed either for fast or slow exit mode
   OR
   a refresh command (issue_refresh_command())
   OR
   a power_up command (issue_powerup_command())
   OR
   an activate to a specific row (issue_activate_command()).

   If a COL-RD or COL-WR is picked for issue, the scheduler also has the
   option to issue an auto-precharge in this cycle (issue_autoprecharge()).

   Before issuing a command it is important to check if it is issuable. For the RD/WR queue resident commands, checking the "command_issuable" flag is necessary. To check if the other commands (mentioned above) can be issued, it is important to check one of the following functions: is_precharge_allowed, is_all_bank_precharge_allowed, is_powerdown_fast_allowed, is_powerdown_slow_allowed, is_powerup_allowed, is_refresh_allowed, is_autoprecharge_allowed, is_activate_allowed.
   */

int max(int a,int b)
{
  if(a>b)
    return a;
  else
    return b;
}

void schedule(int channel)
{
  request_t * rd_ptr = NULL;
  request_t * wr_ptr = NULL;
  request_t * temp_ptr= NULL;
  int check_for_close_page=0;
  request_t * issue_ptr = NULL;

  // if in write drain mode, keep draining writes until the
  // write queue occupancy drops to LO_WM
  if (drain_writes[channel] && (write_queue_length[channel] > LO_WM)) {
    drain_writes[channel] = 1; // Keep draining.
  }
  else {
    drain_writes[channel] = 0; // No need to drain.
  }

  // initiate write drain if either the write queue occupancy
  // has reached the HI_WM , OR, if there are no pending read
  // requests
  if(write_queue_length[channel] > HI_WM)
  {
    drain_writes[channel] = 1;
  }
  else {
    if (!read_queue_length[channel])
      drain_writes[channel] = 1;
  }

  //write drain
  if(drain_writes[channel])
  {

    LL_FOREACH(write_queue_head[channel],wr_ptr)
    { 
      //If there is a thread waiting at Highest priority i.e waited for a long time or a CPU intensive thread, issue that access request
      if(wr_ptr->command_issuable && priority[channel][wr_ptr->thread_id]==Highest_priority)
      {
        issue_ptr=wr_ptr;
        issue_request_command(wr_ptr);
        priority[channel][wr_ptr->thread_id]=0;
        if(wr_ptr->next_command==COL_WRITE_CMD){
          recent_colacc[channel][wr_ptr->dram_addr.rank][wr_ptr->dram_addr.bank]=1;
          check_for_close_page=1;
        }
        break;
      }
      else if(wr_ptr->command_issuable &&  wr_ptr->next_command==COL_WRITE_CMD && temp_ptr==NULL)
      {
        temp_ptr=wr_ptr; // to be used if there is no thread with highest priority and this will have a hit
      }
    }
    if(!command_issued_current_cycle[channel])
    {

      if(temp_ptr!=NULL)
      {
        wr_ptr=temp_ptr;
        issue_ptr=wr_ptr;
        priority[channel][wr_ptr->thread_id]-=2;
        issue_request_command(wr_ptr);
        recent_colacc[channel][wr_ptr->dram_addr.rank][wr_ptr->dram_addr.bank]=1;
        check_for_close_page=1;
      }
      else
      {
        //If no request is found having highest priority or row buffer hit, issue the first issuable request
        LL_FOREACH(write_queue_head[channel], wr_ptr)
        {
          if(wr_ptr->command_issuable)
          {
            issue_ptr=wr_ptr;
            priority[channel][issue_ptr->thread_id]-=2;
            issue_request_command(wr_ptr);
            break;
          }
        }
      }
    }
  }

  // Draining Reads
  // look through the queue and find the request whose
  // command can be issued in this cycle and issue it 
  if(!drain_writes[channel] || !command_issued_current_cycle[channel]) 
  {

    LL_FOREACH(read_queue_head[channel],rd_ptr)
    { 
      //If there is a thread waiting at Highest priority i.e waited for a long time, issue that 
      if(rd_ptr->command_issuable && priority[channel][rd_ptr->thread_id]==Highest_priority)
      {
        issue_ptr=rd_ptr;
        issue_request_command(rd_ptr);
        priority[channel][rd_ptr->thread_id]=0;
        if(rd_ptr->next_command==COL_READ_CMD){
          recent_colacc[channel][rd_ptr->dram_addr.rank][rd_ptr->dram_addr.bank]=1;
          check_for_close_page=1;
        }
        break;
      }
      else if(rd_ptr->command_issuable &&  rd_ptr->next_command==COL_READ_CMD && temp_ptr==NULL)
      {
        temp_ptr=rd_ptr;    // to be used if there is no thread with highest priority and this will have a hit
      }
    }
    if(!command_issued_current_cycle[channel])
    {

      if(temp_ptr!=NULL)
      {
        rd_ptr=temp_ptr;
        issue_ptr=rd_ptr;
        priority[channel][rd_ptr->thread_id]-=2;
        issue_request_command(rd_ptr);
        recent_colacc[channel][rd_ptr->dram_addr.rank][rd_ptr->dram_addr.bank]=1;
        check_for_close_page=1;
      }
      else
      {
        //If no request is found having highest priority or row buffer hit, issue the first issuable request
        LL_FOREACH(read_queue_head[channel],rd_ptr)
        {
          if(rd_ptr->command_issuable)
          {
            issue_ptr=rd_ptr;
            priority[channel][rd_ptr->thread_id]-=2;
            issue_request_command(rd_ptr);
            break;
          }
        }
      }
    }
  }

  //If there was a request issued which had a row buffer hit check if it will have further row buffer hits. If no, auto precharge it
  if(check_for_close_page)
  {
    int Close=1;
    request_t * to_be_rd_ptr;
    request_t * to_be_wr_ptr;
    LL_FOREACH(read_queue_head[channel],to_be_rd_ptr){
      if(to_be_rd_ptr->command_issuable && to_be_rd_ptr->dram_addr.rank==issue_ptr->dram_addr.rank && to_be_rd_ptr->dram_addr.bank==issue_ptr->dram_addr.bank && to_be_rd_ptr->dram_addr.row==issue_ptr->dram_addr.row)
      {
         Close=0;
        break;
      }
    }
    if(Close){
      LL_FOREACH(write_queue_head[channel],to_be_wr_ptr){
        if(to_be_wr_ptr->command_issuable && to_be_wr_ptr->dram_addr.rank==issue_ptr->dram_addr.rank && to_be_wr_ptr->dram_addr.bank==issue_ptr->dram_addr.bank && to_be_wr_ptr->dram_addr.row==issue_ptr->dram_addr.row)
        {  
          Close=0;
          break;
        }
      }
    }

    if(Close)
    {
      if(is_autoprecharge_allowed(channel,issue_ptr->dram_addr.rank,issue_ptr->dram_addr.bank))
      {
        issue_autoprecharge(channel,issue_ptr->dram_addr.rank,issue_ptr->dram_addr.bank);
        numAutoPrecharge+=1;
      }
    }
    //Updating the priority values for all the threads(cores)
    if(command_issued_current_cycle[channel])
    {
      for(int core=0; core < NUMCORES; core++) 
      {
        if(priority[channel][core]<Highest_priority)
        {
          priority[channel][core]+=1;
        }
      }
    }
  }
  
  //  If a command hasn't yet been issued to this channel in this cycle, issue a precharge. 
  if (!command_issued_current_cycle[channel]) {
    for (int i=0; i<NUM_RANKS; i++) {
      for (int j=0; j<NUM_BANKS; j++) {  /* For all banks on the channel.. */
        if (recent_colacc[channel][i][j]) {  /* See if this bank is a candidate. */
          if (is_precharge_allowed(channel,i,j)) {  /* See if precharge is doable. */
      if (issue_precharge_command(channel,i,j)) {
        recent_colacc[channel][i][j] = 0;
      }
    }
        }
      }
    }
  }


}

void scheduler_stats()
{
  /* Nothing to print for now. */
  printf("Number of auto precharges: %lld\n", numAutoPrecharge);
}

