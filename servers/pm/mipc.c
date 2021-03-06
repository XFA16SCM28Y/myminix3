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
#include <stdlib.h>
#include <string.h>
#include <malloc.h>   
#include "mproc.h"
#include "param.h"
#include "mgroup.h"

/* message queue(shared to all groups, because we need detect deadlock) */
/* message queue store proc queues */
static mqueue *msg_queue = NULL;        
static mqueue *block_queue = NULL; 

static mgroup *cur_group;               /* current message group*/

/* private methods prototype */
int deadlock(mgroup *g_ptr, int call_nr);                   /* valid deadlock */ 
int getgroup(int grp_nr, mgroup ** g_ptr);                  /* get group by its gid */
int getprocindex(mgroup *g_ptr, int proc);                  /* get proc index in group*/
endpoint_t getendpoint(int proc_id);                        /* get endpoint from proc list*/
void unblock(endpoint_t proc_e, message *msg);              /* unblock a process */
int searchinproc(mqueue *proc_q, grp_message *g_m);         /* search send->rec chain from proc */
void deadlock_addpend(mqueue *proc_q, mqueue *pend_q, int call_nr);  /*recursive detect deadlock */
int getprocqueue(endpoint_t proc_e, mqueue **proc_q);       /* get proc queue from msg_queue */
int getblockproc(endpoint_t proc_e, mqueue **block_q);      /* get proc queue from block_queue */

int svr_recover(mgroup *g_ptr, int strategy){
    switch(strategy){
        case IGNORE_ELOCK:
            // Clear all invalid chain.
            while(queue_func->dequeue(&value, g_ptr->invalid_q));
            do_server_ipc();
            break;
        case CANCEL_IPC:
            while(queue_func->dequeue(&value, g_ptr->invalid_q));
            while(queue_func->dequeue(&value, g_ptr->pending_q));
            while(queue_func->dequeue(&value, g_ptr->valid_q));
            notify(g_ptr->flag);               //unblock sender.
            break;
        case CLEAR_MSG:
            break;
        case CLEAR_ALL_MSG:
            //TODO
            break;
    }
    g_ptr->g_stat = M_READY;
    release_lock(g_ptr);
    return 0;
}

int do_msend(){
    int rv=SUSPEND, src, caller, grp_nr, ipc_type, i, s;
    message *msg;
    mgroup *g_ptr = NULL;
    grp_message *g_m, *msg_m;
    mqueue *proc_q;
    void *value;
    
    // Init and valid
    caller = m_in.m1_i1;
    grp_nr = m_in.m1_i2;
    ipc_type = m_in.m1_i3;
    msg = (message*) malloc(sizeof(message));
    if(getgroup(grp_nr, &g_ptr) == -1){
        return EIVGRP;
    } else if((src=getendpoint(caller)) < 0){
        return EIVPROC;
    } else if(getprocindex(g_ptr, src) == -1){
        return ENOPROC;
    }
    
    // Copy data from m_in.
    if ((message *) m_in.m1_p1 != (message *) NULL) {
        rv = sys_datacopy(who_e, (vir_bytes) m_in.m1_p1,
            PM_PROC_NR, (vir_bytes) msg, (phys_bytes) sizeof(message));
        if (rv != OK) return(rv);
    } 
    
    // add a new message by different ipc_type.
    acquire_lock(g_ptr);
    
    cur_group = g_ptr;
    g_ptr->g_stat = M_SENDING;
    switch(ipc_type){
        case SENDALL:
            for(i=0; i<g_ptr->p_size; i++){
                if(src != g_ptr->p_lst[i]){
                    g_m = (grp_message *)malloc(sizeof(grp_message));
                    g_m->group=g_ptr;
                    g_m->sender=src;
                    g_m->receiver=g_ptr->p_lst[i];
                    g_m->call_nr=SEND;
                    g_m->msg= msg;
                    g_m->ipc_type = ipc_type;
                    queue_func->enqueue(g_m, g_ptr->pending_q);
                }
            }
            break;
        case IPCTOREQ:
            if(getprocqueue(src, &proc_q) > -1){    //Only send when proc queue exist
                queue_func->iterator(proc_q);
                while(queue_func->next(&value, proc_q)){
                    msg_m = (grp_message *)value;
                    if(msg_m->call_nr == RECEIVE){
                        g_m = (grp_message *)malloc(sizeof(grp_message));
                        g_m->group=g_ptr;
                        g_m->sender=src;
                        g_m->receiver=msg_m->receiver;
                        g_m->call_nr=SEND;
                        g_m->msg= msg;
                        g_m->ipc_type = ipc_type;
                        queue_func->enqueue(g_m, g_ptr->pending_q);
                    }
                }
            }
            break;
        case IPCNONBLOCK:
            for(i=0; i<g_ptr->p_size; i++){
                if(src != g_ptr->p_lst[i]){
                    s = getprocqueue(g_ptr->p_lst[i], &proc_q);
                    if(s < 0 || proc_q->size == 0){
                        g_m = (grp_message *)malloc(sizeof(grp_message));
                        g_m->group=g_ptr;
                        g_m->sender=src;
                        g_m->receiver=g_ptr->p_lst[i];
                        g_m->call_nr=SEND;
                        g_m->msg= msg;
                        g_m->ipc_type = ipc_type;
                        queue_func->enqueue(g_m, g_ptr->pending_q);
                    }
                }
            }
            break;
        default: 
            if((s=getendpoint(ipc_type)) < 0) return s; 
            g_m = (grp_message *)malloc(sizeof(grp_message));
            g_m->group=g_ptr;
            g_m->sender=src;
            g_m->receiver=s;
            g_m->call_nr=SEND;
            g_m->msg= msg;
            g_m->ipc_type = ipc_type;
            queue_func->enqueue(g_m, g_ptr->pending_q);
    }
    
    // return value
    if(queue_func->isempty(g_ptr->pending_q)) return NOIPCOP;
    if(ipc_type != IPCTOREQ)  rv = deadlock(g_ptr, SEND);                              // detect deadlock
    return rv == 0 ? SUSPEND : rv;
}

int do_mreceive(){
    int rv=SUSPEND, src, caller, grp_nr, ipc_type, i, s;
    mgroup *g_ptr = NULL;
    grp_message *g_m, *msg_m;
    mqueue *proc_q;
    void *value;
    
    // Init and valid
    caller = m_in.m1_i1;
    grp_nr = m_in.m1_i2;
    ipc_type = m_in.m1_i3;
    if(getgroup(grp_nr, &g_ptr) == -1){
        return EIVGRP;
    } else if((src=getendpoint(caller)) < 0){
        return EIVPROC;
    } else if(getprocindex(g_ptr, src) == -1){
        return ENOPROC;
    }
    
    // receive a new message by different ipc_type.
    cur_group = g_ptr;
    g_ptr->g_stat = M_RECEIVING;
    
    acquire_lock(g_ptr);

    switch(ipc_type){
        case RECANY:
            for(i=0; i<g_ptr->p_size; i++){
                if(src != g_ptr->p_lst[i] && getprocqueue(g_ptr->p_lst[i], &proc_q) > -1 && proc_q->size > 0){
                    queue_func->iterator(proc_q);
                    while(queue_func->next(&value, proc_q)){
                        msg_m = (grp_message *)value;
                        if(msg_m->call_nr == SEND && msg_m->receiver==src){
                            g_m = (grp_message *)malloc(sizeof(grp_message));
                            g_m->group=g_ptr;
                            g_m->receiver=src;
                            g_m->sender=g_ptr->p_lst[i];
                            g_m->call_nr=RECEIVE;                                    
                            queue_func->enqueue(g_m, g_ptr->pending_q);
                            return SUSPEND;
                        }
                    }
                }
            }
            return NOIPCOP;
        case IPCTOREQ:
            for(i=0; i<g_ptr->p_size; i++){
                if(src != g_ptr->p_lst[i] && getprocqueue(g_ptr->p_lst[i], &proc_q) > -1 && proc_q->size > 0){
                    queue_func->iterator(proc_q);
                    while(queue_func->next(&value, proc_q)){
                        msg_m = (grp_message *)value;
                        if(msg_m->call_nr == SEND && msg_m->receiver==src){
                            g_m = (grp_message *)malloc(sizeof(grp_message));
                            g_m->group=g_ptr;
                            g_m->receiver=src;
                            g_m->sender=g_ptr->p_lst[i];
                            g_m->call_nr=RECEIVE;                                    
                            queue_func->enqueue(g_m, g_ptr->pending_q);
                            break;
                        }
                    }
                }
            }
            break;
        default:
            if((s=getendpoint(ipc_type)) < 0) return s; 
            g_m = (grp_message *)malloc(sizeof(grp_message));
            g_m->group=g_ptr;
            g_m->receiver=src;
            g_m->sender=s;
            g_m->call_nr=RECEIVE;
            queue_func->enqueue(g_m, g_ptr->pending_q);
    }        
    
    // return value
    if(queue_func->isempty(g_ptr->pending_q)) return NOIPCOP;
    if(ipc_type != IPCTOREQ)  rv = deadlock(g_ptr, RECEIVE);                              // detect deadlock
    return rv == 0 ? SUSPEND : rv;
}

/*
 * Check message queue, when find match grp_message, send reply to its src & dest, then unblock both of them.
 */
void do_server_ipc(){
    int rv=0, flag;
    mqueue *proc_q;
    void *value;
    grp_message *g_m;
    
    while(queue_func->dequeue(&value, cur_group->valid_q)){
         g_m = (grp_message *)value;
         queue_func->iterator(msg_queue);
         flag = 0;          
         while(queue_func->next(&value, msg_queue)){
            proc_q = (mqueue *)value;
             /* find match proc*/
            if(searchinproc(proc_q, g_m) > 0) {
                flag = 1;
                break;
            }
         }
         
         /* if not find match proc */
         if(flag == 0){
             // create a new proc in queue, and enqueue its first item.
             initqueue(&proc_q);
             proc_q->number = g_m->sender;
             queue_func->enqueue(g_m, proc_q);
             queue_func->enqueue(proc_q, msg_queue);
         }
    }
    
    // Release lock
    cur_group->g_stat = M_READY;
    release_lock(cur_group);
}

void do_deadlock(){
    // Reserved, we did not do any action until now. (wait user choose any strategy to recover)
}

void do_errohandling(){
    release_lock(cur_group);
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
    
    if(grp_nr < 0 || grp_nr>=NR_GRPS){
        return -1;
    }
    
    for(i=0, k=g_nr_ptr; i<NR_GRPS; i++, k--){
        k=(k+NR_GRPS)%NR_GRPS;
        if(mgrp[k].g_stat != M_UNUSED && mgrp[k].g_nr == grp_nr){       // find the group in group table.
            (*g_ptr) = &mgrp[k];
            return 0;
        }
    }
    return -1;
}

int invalid(int strategy){
    return 0;
}

endpoint_t getendpoint(int proc_id){
    register struct mproc *rmp;
    if(proc_id < 0){
        return -1;
    }
    for (rmp = &mproc[NR_PROCS-1]; rmp >= &mproc[0]; rmp--){ 
        if (!(rmp->mp_flags & IN_USE)) continue;
        if (proc_id > 0 && proc_id == rmp->mp_pid) return rmp->mp_endpoint;
    }
    return -1;
}

void unblock(endpoint_t proc_e, message *msg){
    register struct mproc *rmp;
    int s, proc_nr;
    
    for (rmp = &mproc[NR_PROCS-1]; rmp >= &mproc[0]; rmp--){ 
        if (!(rmp->mp_flags & IN_USE)) continue;
        if(proc_e == rmp->mp_endpoint){
            if (pm_isokendpt(rmp->mp_endpoint, &proc_nr) != OK) {
                panic("handle_vfs_reply: got bad endpoint from VFS: %d", rmp->mp_endpoint);
            }
            memcpy(&rmp->mp_reply, msg, sizeof(message));
            setreply(proc_nr, OK);
            if ((rmp->mp_flags & (REPLY | IN_USE | EXITING)) == (REPLY | IN_USE)) {
              s=sendnb(rmp->mp_endpoint, &rmp->mp_reply);
              if (s != OK) {
                  printf("PM can't reply to %d (%s): %d\n",
                      rmp->mp_endpoint, rmp->mp_name, s);
              }
              rmp->mp_flags &= ~REPLY;
            }
        }
    }
}

int deadlock(mgroup *g_ptr, int call_nr){
    grp_message *g_m;
    mqueue *proc_q, *valid_q, *pend_q, *invalid_q;
    int rv = 0, deadlock, dest_e;
    
    // Iterative valid pending_q
    while(queue_func->dequeue((grp_message *)&g_m, g_ptr->pending_q)){
        initqueue(&valid_q);
        initqueue(&pend_q);
        deadlock = 0;
        
        if(getprocqueue(g_m->receiver, &proc_q) != -1){
            queue_func->enqueue(g_m->receiver, pend_q);
            while(queue_func->dequeue(&dest_e, pend_q)){
                queue_func->enqueue(dest_e, valid_q);                            // Put cur process into already 
                if(getprocqueue(dest_e, &proc_q) != -1){
                    deadlock_addpend(proc_q, pend_q, call_nr);
                }
            }
            if(queue_func->hasvalue((void *)g_m->sender, valid_q)){                  // if sender exist in the dest
                g_ptr->flag = who_e;
                g_ptr->g_stat = M_DEADLOCK;                                          //Deadlock
                deadlock = 1;
                rv = ELOCKED;                                                       
            }
        }
        
        if(!deadlock){
            queue_func->enqueue(g_m, g_ptr->valid_q);
        } else {
            printf("Deadlock occur: %d(endpoint)->%d(endpoint)\n", g_m->sender, g_m->receiver);         // Print deadlock
            queue_func->enqueue(g_m, g_ptr->invalid_q);                          //input invalid_queue
        }
        closequeue(valid_q);
        closequeue(pend_q);
    }
    
    return rv;
}

/*
 * Add all dest into pend_q
 */
void deadlock_addpend(mqueue *proc_q, mqueue *pend_q, int call_nr){
    grp_message *msg_m;
    
    // Put all receiver into pend_q from current proc.
    queue_func->iterator(proc_q);
    while(queue_func->next(&msg_m, proc_q)){
        if(msg_m->call_nr != call_nr) continue;
        queue_func->enqueue((void *)msg_m->receiver, pend_q);
    }
}

/*
 * search msg match from queues.
 */
int searchinproc(mqueue *proc_q, grp_message *g_m){
    grp_message *msg_m;
    message *msg;
    int receiverNum = 0, rv = 0;
    void *value;
    
    if(g_m->sender == proc_q->number){                              //Only check/store sender. do not need check twice: sender and receiver
        queue_func->iterator(proc_q);
        while(queue_func->next(&value, proc_q)){
            msg_m=(grp_message *)value;
            if(msg_m->call_nr == SEND) receiverNum++;
            if(msg_m->call_nr == g_m->call_nr) continue;           // Only search send->receive
             // If sender and receiver match: sender = sender, callnr = SEND+RECEIVE, receiver = receiver
            if(msg_m->receiver == g_m->receiver){
                msg = msg_m->call_nr == SEND ? msg_m->msg : g_m->msg;
                
                unblock(msg_m->receiver, msg);
                
                
//                unblock(msg_m->sender, msg);

                queue_func->removeitem(proc_q);              //Remove current message from proc_queue(not proc)
//                free(msg_m);
//                free(g_m);
                rv = 2;
            } 
        }
        if(rv != 2){
            queue_func->enqueue(g_m, proc_q);                   //If not match, then enqueue this message.
            rv = 1;
        } else {
            printf("still need %d \n", receiverNum);
            if(receiverNum <= 1){
                unblock(g_m->sender, msg);
            }
        }
    }
    return 0;
}

int getprocqueue(endpoint_t proc_e, mqueue **proc_q){
    void *value;
    mqueue *q;
    
    queue_func->iterator(msg_queue);
    while(queue_func->next(&value, msg_queue)){
        q = (mqueue *)value;
        if(q->number == proc_e){
            (*proc_q) = q;
            return 0;
        }
    }
    return -1;
}

int acquire_lock(mgroup *g_ptr){
    while(g_ptr->lock != 0);
    g_ptr->lock = 1;       // Enter critical region.
}

int release_lock(mgroup *g_ptr){
    g_ptr->lock = 0;
}
