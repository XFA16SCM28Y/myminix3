#include "stdio.h"
#include "pm.h"
#include <sys/wait.h>
#include <assert.h>
#include <minix/callnr.h>
#include <minix/com.h>
#include <minix/sched.h>
#include <minix/vm.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <signal.h>
#include "mproc.h"
#include "param.h"
#include "mgroup.h"

static mgroup mgrp[NR_GRPS];            /* group table [this design is similar to proc design in minix] */
static int g_nr_ptr = 0;                /* group number ptr */
static int g_id_ctr = 0;                /* group id counter */

/* private methods prototype */
int valid(int strategy);                        /* valid strategy */
int getgroup(int grp_nr, mgroup ** g_ptr);      /* get group by its gid */
int getprocindex(mgroup *g_ptr, int proc);      /* get proc index in group*/

int do_opengroup()
{
    mgroup *g_ptr = NULL;
    int i, strategy;
    
    strategy = m_in.m1_i1;
    if(!valid(strategy)){                           // Make sure strategy is valid. 0 is allowed
        return -1;
    }
    for(i=0; i<NR_GRPS; i++, g_nr_ptr++){
        g_nr_ptr %= NR_GRPS;                        // Circle detect.
        if(mgrp[g_nr_ptr].g_stat == M_UNUSED){      // This group is not used until now.
            g_ptr = mgrp + g_nr_ptr；
            break;
        }
    }
    if(g_ptr == NULL){                              // No avalible(free) group in PM server.
        return -1;                          
    }
    
    g_ptr->grp_stat = M_READY;
    g_ptr->g_nr = g_id_ctr;
    g_ptr->g_sttg = strategy;
    g_ptr->p_size = 0;
    g_id_ctr++;
    
    return g_ptr->g_nr;
}

int do_addproc(){
    mgroup *g_ptr = NULL;
    int grp_nr, proc;	
    
    grp_nr = m_in.m1_i1;
    proc = m_in.m1_i2;
    if(getgroup(grp_nr, &g_ptr)){
        return -1;
    }else if(g_ptr->p_size == NR_MGPROCS){
        return -1;
    }else if(getprocindex(g_ptr, proc) != -1){
        return -1;
    }
    
    *(g_ptr->p_lst+g_ptr->p_size) = proc;
    g_ptr->p_size++;
    return 0;
}

int do_rmproc(){
    mgroup *g_ptr = NULL;
    int i, grp_nr, proc;
    
    grp_nr = m_in.m1_i1;
    proc = m_in.m1_i2;
    if(getgroup(grp_nr, &g_ptr)){
        return -1;
    } else if((i=getprocindex(g_ptr, proc)) == -1){
        return -1;
    } 
    
    for(; i<g_ptr->p_size-1;i++){
        *(g_ptr->p_lst+i) = *(g_ptr->p_lst+i+1);
    }
    g_ptr->p_size--;
    return 0;
} 

int do_closegroup(){
    mgroup *g_ptr = NULL;
    int i, grp_nr;
    
    grp_nr = m_in.m1_i1;
    if(getgroup(grp_nr, &g_ptr)){
        return -1;
    }
    g_ptr->grp_stat = M_UNUSED;
    g_ptr->g_nr = 0;
    g_ptr->g_sttg = 0;
    g_ptr->p_size = 0;
    
    return 0;
}

int do_recovergroup(){
    mgroup *g_ptr = NULL;
    int i, grp_nr, strategy;
    
    grp_nr = m_in.m1_i1;
    strategy = m_in.m1_i2;
    if(!valid(strategy)){                           // Make sure strategy is valid. 0 is allowed
        return -1;
    }else if(getgroup(grp_nr, &g_ptr)){
        return -1;
    }
    
    return 0;
}

int do_msend(int g_dst, message *m_ptr){
    return 0;
}

int do_mreceive(int g_src, message *m_ptr, int *status_ptr){
    return 0;
}

/*  ========================= private methods ================================*/

int getprocindex(mgroup *g_ptr, int proc){
    int i;
    
    for(i=0; i<g_ptr->p_size;i++){
        if(*(g_ptr->p_lst+i) == proc){
            return i;
        }
    }
    return -1;
}

int getgroup(int grp_nr, mgroup ** g_ptr){
    int i, k;
    
    for(i=0, k=g_nr_ptr; i<NR_GRPS; i++, k--){
        k=(k+NR_GRPS)%NR_GRPS;
        if(mgrp[k].g_stat != M_UNUSED && mgrp[k].g_nr == grp_nr){       // find the group in group table.
            (*g_ptr) = &mgrp[k];
            return 0;
        }
    }
    return -1;
}

int valid(int strategy){
    return 0;
}