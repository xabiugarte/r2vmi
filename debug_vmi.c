#include <r_asm.h>
#include <r_debug.h>
#include <strings.h>

#include "io_vmi.h"
#include "utils.h"

// vmi_events_listen loop
static bool interrupted = false;

//
// callbacks
//
static event_response_t cb_on_mem_event(vmi_instance_t vmi, vmi_event_t *event){
    status_t status;
    bp_event_data *event_data;
    const char *pname = NULL;
    RIOVmi *rio_vmi = NULL;

    eprintf("%s\n", __func__);

    if(!event || event->type != VMI_EVENT_MEMORY || !event->data) {
        eprintf("ERROR (%s): invalid event encounted\n", __func__);
        return VMI_EVENT_RESPONSE_NONE;
    }

    // get event_data
    event_data = (bp_event_data*) event->data;
    rio_vmi = event_data->rio_vmi;

    // did we catched a memory event while the user was simply asking for a singlestep ?
    if (rio_vmi->cmd_sstep)
    {
        // clear the memory event
        // so that the singlestep event can work
        status = vmi_clear_event(vmi, event, NULL);
        if (VMI_FAILURE == status)
            eprintf("%s: fail to clear event\n", __func__);
        return VMI_EVENT_RESPONSE_NONE;
    }

    pname = dtb_to_pname(vmi, event->x86_regs->cr3);

    // our pid ?
    if (event->x86_regs->cr3 != event_data->pid_cr3)
    {
        eprintf("%s: wrong cr3 (%s)(0x%lx)\n", __func__, pname, event->x86_regs->cr3);
        return VMI_EVENT_RESPONSE_EMULATE;
    }

    // at the right rip ?
    if (!vaddr_equal(vmi, event->x86_regs->rip, event_data->bp_vaddr))
    {
        eprintf("%s: wrong rip: %"PRIx64" (bp_vaddr: %"PRIx64")\n", __func__, event->x86_regs->rip, event_data->bp_vaddr);
        return VMI_EVENT_RESPONSE_EMULATE;
    }

    eprintf("%s: RIP: %"PRIx64 " (%s)\n", __func__, event->x86_regs->rip, pname);
    print_event(event);

    // pause VM
    status = vmi_pause_vm(vmi);
    if (VMI_FAILURE == status)
        eprintf("%s: Fail to pause vm\n", __func__);

    // stop listen
    interrupted = true;

    return 0;
}

static event_response_t cb_on_sstep(vmi_instance_t vmi, vmi_event_t *event) {
    status_t status;
    bp_event_data *event_data = NULL;

    eprintf("%s\n", __func__);

    if(!event || event->type != VMI_EVENT_SINGLESTEP) {
        eprintf("ERROR (%s): invalid event encounted\n", __func__);
        return 0;
    }

    // event data ?
    if (event->data)
    {
        // coming from software breakpoint
        event_data = (bp_event_data*) event->data;
        // restore software breakpoint
        r_bp_restore_one(event_data->bp, event_data->bpitem, true);
        // null event data
        event->data = NULL;
        // toggle singlestep OFF
        return VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP;
    }
    else
    {
        // simple singlestep
        // stop monitoring
        interrupted = true;
        // pause the VM before exiting the callback
        status = vmi_pause_vm(vmi);
        if (status == VMI_FAILURE)
            eprintf("%s: Fail to pause VM\n", __func__);

        return VMI_EVENT_RESPONSE_NONE;
    }
}

static event_response_t cb_on_int3(vmi_instance_t vmi, vmi_event_t *event){
    status_t status;
    bp_event_data *event_data;
    char *proc_name = NULL;
    RIOVmi *rio_vmi = NULL;

    eprintf("%s\n", __func__);

    if(!event || event->type != VMI_EVENT_INTERRUPT || !event->data) {
        eprintf("ERROR (%s): invalid event encounted\n", __func__);
        return VMI_EVENT_RESPONSE_NONE;
    }

    // get event_data
    event_data = (bp_event_data*) event->data;
    rio_vmi = event_data->rio_vmi;

    // process name
    proc_name = dtb_to_pname(vmi, event->x86_regs->cr3);

    // default reinject behavior
    // do not reinject interrupt in the guest*
    // TODO check list of breakpoints from r2
    event->interrupt_event.reinject = 0;

    // our targeted process ?
    if (event->x86_regs->cr3 != event_data->pid_cr3)
    {
        eprintf("%s: wrong process %s (0x%lx)\n", __func__, proc_name, event->x86_regs->cr3);

        // add event data to singlestep event already registered
        rio_vmi->sstep_event->data = event->data;

        // restore original opcode
        r_bp_restore_one(event_data->bp, event_data->bpitem, false);

        // toggle singlestep ON
        return VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP;
    }
    else
    {
        // pause VM
        status = vmi_pause_vm(vmi);
        if (VMI_FAILURE == status)
        {
            eprintf("%s: Fail to pause vm\n", __func__);
        }
        // stop listen
        interrupted = true;

        return VMI_EVENT_RESPONSE_NONE;
    }
}


//
// R2 debug interface
//
static int __step(RDebug *dbg) {
    RIODesc *desc = NULL;
    RIOVmi *rio_vmi = NULL;
    status_t status;

    eprintf("%s\n", __func__);

    desc = dbg->iob.io->desc;
    rio_vmi = desc->data;
    if (!rio_vmi)
    {
        eprintf("%s: Invalid RIOVmi\n", __func__);
        return 1;
    }

    // enable singlestep
    // hack around lack of API in LibVMI
    // clear current event
    status = vmi_clear_event(rio_vmi->vmi, rio_vmi->sstep_event, NULL);
    if (VMI_FAILURE == status)
    {
        eprintf("%s: fail to clear event\n", __func__);
        return false;
    }

    // resetup singlestep event, enabled
    bzero(rio_vmi->sstep_event, sizeof(vmi_event_t));
    SETUP_SINGLESTEP_EVENT(rio_vmi->sstep_event, 1u << 0, cb_on_sstep, true);
    // clear data field (not a software breakpoint)
    rio_vmi->sstep_event->data = NULL;
    // register event
    status = vmi_register_event(rio_vmi->vmi, rio_vmi->sstep_event);
    if (status == VMI_FAILURE)
    {
        eprintf("%s: fail to register event\n", __func__);
        return false;
    }

    status = vmi_resume_vm(rio_vmi->vmi);
    if (status == VMI_FAILURE)
    {
        eprintf("%s: Failed to resume VM execution\n", __func__);
        return false;
    }

    // we are in the command singlestep
    rio_vmi->cmd_sstep = true;

    return true;
}


// "dc" continue execution
static int __continue(RDebug *dbg, __attribute__((unused)) int pid, __attribute__((unused)) int tid, __attribute__((unused)) int sig) {
    RIODesc *desc = NULL;
    RIOVmi *rio_vmi = NULL;
    status_t status;

    eprintf("%s, sig: %d\n", __func__, sig);

    desc = dbg->iob.io->desc;
    rio_vmi = desc->data;
    if (!rio_vmi)
    {
        eprintf("%s: Invalid RIOVmi\n", __func__);
        return 1;
    }

    status = vmi_resume_vm(rio_vmi->vmi);
    if (VMI_FAILURE == status)
    {
        eprintf("%s: Failed to resume VM execution\n", __func__);
        return 1;
    }

    return 0;
}

static int __attach(RDebug *dbg, int pid) {
    RIODesc *desc = NULL;
    RIOVmi *rio_vmi = NULL;
    status_t status = 0;

    eprintf("Attaching to pid %d...\n", pid);

    desc = dbg->iob.io->desc;
    rio_vmi = desc->data;
    if (!rio_vmi)
    {
        eprintf("%s: Invalid RIOVmi\n", __func__);
        return 1;
    }

    intercept_process(dbg, pid);

    // set attached to allow reg_read
    rio_vmi->attached = true;

    rio_vmi->sstep_event = R_NEW0(vmi_event_t);

    // did we attached to a new process ?
    if (rio_vmi->attach_new_process)
    {
        return attach_new_process(dbg);
    }
    else
    {
        eprintf("Attaching to existing process is not implemented\n");
    }

    // init singlestep event (not enabled)
    SETUP_SINGLESTEP_EVENT(rio_vmi->sstep_event, 1u << 0, cb_on_sstep, false);
    // register event
    status = vmi_register_event(rio_vmi->vmi, rio_vmi->sstep_event);
    if (VMI_FAILURE == status)
    {
        eprintf("%s: fail to register event\n", __func__);
        return 1;
    }

    return 0;
}

static int __detach(__attribute__((unused)) RDebug *dbg, __attribute__((unused)) int pid) {
    eprintf("%s\n", __func__);

    return 1;
}

static RList* __threads(__attribute__((unused)) RDebug *dbg, __attribute__((unused)) int pid) {
    eprintf("%s\n", __func__);

    return NULL;
}

static RDebugReasonType __wait(RDebug *dbg, __attribute__((unused)) int pid) {
    RDebugReasonType reason = R_DEBUG_REASON_UNKNOWN;
    RIODesc *desc = NULL;
    RIOVmi *rio_vmi = NULL;
    status_t status;
    eprintf("%s\n", __func__);

    desc = dbg->iob.io->desc;
    rio_vmi = desc->data;
    if (!rio_vmi)
    {
        eprintf("%s: Invalid RIOVmi\n", __func__);
        return reason;
    }

    // handle CTRL-C
    r_cons_break_push(NULL, NULL);

    interrupted = false;
    while (!interrupted && !r_cons_is_breaked()) {
        eprintf("%s: Listen to VMI events...\n", __func__);
        status = vmi_events_listen(rio_vmi->vmi, 1000);
        if (status == VMI_FAILURE)
        {
            eprintf("%s: Fail to listen to events\n", __func__);
            return reason;
        }
    }
    // exit because of CTRL-C ?
    if (r_cons_is_breaked())
    {
        eprintf("CTRL-C !\n");
        intercept_process(dbg, rio_vmi->pid);
    }
    r_cons_break_pop();

    // clear event buffer if any
    status = vmi_events_listen(rio_vmi->vmi, 0);
    if (status == VMI_FAILURE)
    {
        eprintf("%s: fail to clear event buffer\n", __func__);
        return reason;
    }

    // clear event if singlestep
    // breakpoint events are cleared in __breakpoint if unset
    // was it a single step ?
    if (rio_vmi->cmd_sstep)
    {
        // hack around lack of API in LibVMI
        status = vmi_clear_event(rio_vmi->vmi, rio_vmi->sstep_event, NULL);
        if (VMI_FAILURE == status)
        {
            eprintf("%s: fail to clear event\n", __func__);
            return false;
        }

        // set singlestep event, disabled
        bzero(rio_vmi->sstep_event, sizeof(vmi_event_t));
        SETUP_SINGLESTEP_EVENT(rio_vmi->sstep_event, 1u << 0, cb_on_sstep, false);
        rio_vmi->sstep_event->data = NULL;
        // register it
        status = vmi_register_event(rio_vmi->vmi, rio_vmi->sstep_event);
        if (VMI_FAILURE == status)
        {
            eprintf("%s: fail to register event\n", __func__);
            return false;
        }

        rio_vmi->cmd_sstep = false;

        reason = R_DEBUG_REASON_STEP;
    }
    else
    {
        reason = R_DEBUG_REASON_BREAKPOINT;
    }

    return reason;
}

// "dm" get memory maps of target process
static RList *__map_get(RDebug* dbg) {
    RIODesc *desc = NULL;
    RIOVmi *rio_vmi = NULL;
    status_t status;
    addr_t dtb = 0;
    page_mode_t page_mode;
    char unknown[] = "unknown_";

    eprintf("%s\n", __func__);

    desc = dbg->iob.io->desc;
    rio_vmi = desc->data;
    if (!rio_vmi)
    {
        eprintf("%s: Invalid RIOVmi\n", __func__);
        return NULL;
    }

    status = vmi_pid_to_dtb(rio_vmi->vmi, rio_vmi->pid, &dtb);
    if (status == VMI_FAILURE)
    {
        eprintf("Fail to get dtb from pid\n");
        return NULL;
    }

    page_mode = vmi_get_page_mode(rio_vmi->vmi, rio_vmi->current_vcpu);

    GSList *va_pages = vmi_get_va_pages(rio_vmi->vmi, dtb);
    if (!va_pages)
    {
        eprintf("Fail to get va pages\n");
        return NULL;
    }


    RList *r_maps = r_list_newf((RListFree) r_debug_map_free);
    GSList *loop = va_pages;
    int nb = 0;
    while (loop)
    {
        addr_t pte_value = 0;
        page_info_t *page = loop->data;
        int permissions = R_PERM_R;
        int supervisor = 0;
        char str_nb[20];

        // new map name
        int str_nb_size = sprintf(str_nb, "%d", nb);
        char *map_name = calloc(strlen(unknown) + str_nb_size + 1, 1);
        strncat(map_name, unknown, strlen(map_name));
        strncat(map_name, str_nb, str_nb_size);
        // get permissions
        switch (page_mode) {
        case VMI_PM_LEGACY:
            pte_value = page->x86_legacy.pte_value;
            break;
        case VMI_PM_PAE:
            pte_value = page->x86_pae.pte_value;
            if (!VMI_GET_BIT(pte_value, 63))
                permissions |= R_PERM_X;
            break;
        case VMI_PM_IA32E:
            pte_value = page->x86_ia32e.pte_value;
            break;
        default:
            eprintf("Unhandled page mode");
            // TODO free
            return NULL;
        }
        supervisor = USER_SUPERVISOR(pte_value);
        if (READ_WRITE(pte_value))
            permissions |= R_PERM_W;
        // build RDebugMap
        addr_t map_start = page->vaddr;
        addr_t map_end = page->vaddr + page->size;
        RDebugMap *r_debug_map = r_debug_map_new (map_name, map_start, map_end, permissions, supervisor);
        // append
        r_list_append (r_maps, r_debug_map);
        // loop
        loop = loop->next;
        nb +=1;
    }

    // free va_pages
    while (va_pages)
    {
        g_free(va_pages->data);
        va_pages = va_pages->next;
    }
    g_slist_free(va_pages);

    return r_maps;
}

static RList* __modules_get(__attribute__((unused)) RDebug *dbg) {
    eprintf("%s\n", __func__);

    return NULL;
}

static int __breakpoint (struct r_bp_t *bp, RBreakpointItem *b, bool set) {
    RIODesc *desc = NULL;
    RIOVmi *rio_vmi = NULL;
    status_t status;
    addr_t bp_vaddr = b->addr;
    gboolean ret;
    // return value of this function
    // whether our implementation handled the breakpoint
    // or if r2 should do it
    bool bp_handled = false;
    vmi_event_t *bp_event = NULL;
    eprintf("%s, set: %d, addr: %"PRIx64", hw: %d\n", __func__, set, bp_vaddr, b->hw);
    if (!bp)
        return false;

    desc = bp->iob.io->desc;
    rio_vmi = (RIOVmi*) desc->data;

    if (set)
    {
        // the breakpoint API will be called multiple times for the same breakpoint
        // in case of single stepping for example, radare2 still calls this API
        // for each breakpoint before the single-step
        // therefore, check if the breakpoint has already been inserted

        bp_event = (vmi_event_t*) g_hash_table_lookup(rio_vmi->bp_events_table, GINT_TO_POINTER(bp_vaddr));
        if (!bp_event)
        {
            if (b->hw)
            {
                // hardware breakpoint
                // need to translate the virtual address to physical
                addr_t paddr;
                status = vmi_translate_uv2p(rio_vmi->vmi, bp_vaddr, rio_vmi->pid, &paddr);
                if (VMI_FAILURE == status)
                {
                    eprintf("Fail to get physical addresss\n");
                    return 1;
                }

                // get guest frame number
                addr_t gfn = paddr >> 12;
                eprintf("%s: paddr: %016"PRIx64", gfn: %"PRIx64"\n", __func__, paddr, gfn);

                // prepare new vmi_event
                bp_event = R_NEW0(vmi_event_t);
                if (!bp_event)
                {
                    eprintf("%s: Fail to allocate memory\n", __func__);
                    return false;
                }
                SETUP_MEM_EVENT(bp_event, gfn, VMI_MEMACCESS_X, cb_on_mem_event, 0);
                bp_handled = true;
            }
            else
            {
                // software breakpoint
                // prepare new vmi_event
                bp_event = R_NEW0(vmi_event_t);
                if (!bp_event)
                {
                    eprintf("%s: Fail to allocate memory\n", __func__);
                    return false;
                }
                SETUP_INTERRUPT_EVENT(bp_event, cb_on_int3);
                // r2 has to write the software breakpoint by himself
                bp_handled = false;
            }
            // add event data
            bp_event_data *event_data = R_NEW0(bp_event_data);
            if (!event_data)
            {
                eprintf("%s: Fail to allocate memory\n", __func__);
                return false;
            }
            event_data->pid_cr3 = rio_vmi->pid_cr3;
            event_data->bp_vaddr = bp_vaddr;
            event_data->bp = bp;
            event_data->bpitem = b;
            event_data->rio_vmi = rio_vmi;
            bp_event->data = event_data;

            // add our breakpoint to the hashtable
            // [bp_vaddr] -> [vmi_event *]
            ret = g_hash_table_insert(rio_vmi->bp_events_table, GINT_TO_POINTER(bp_vaddr), bp_event);
            if (FALSE == ret)
            {
                eprintf("%s: Fail to insert event into ghashtable\n", __func__);
                return false;
            }

            // register breakpoint event
            // either interrupt or mem event
            status = vmi_register_event(rio_vmi->vmi, bp_event);
            if (VMI_FAILURE == status)
            {
                eprintf("%s: Fail to register event\n", __func__);
                return false;
            }
        }
    } else {
        // unset
        // get event from ghashtable
        bp_event = (vmi_event_t*) g_hash_table_lookup(rio_vmi->bp_events_table, GINT_TO_POINTER(bp_vaddr));
        if (bp_event)
        {
            // unregister event
            status = vmi_clear_event(rio_vmi->vmi, bp_event, NULL);
            if (VMI_FAILURE == status)
            {
                eprintf("%s: Fail to clear event\n", __func__);
                return false;
            }
            if (bp_event->data)
                free(bp_event->data);
            free(bp_event);

            // remove key/value from table
            ret = g_hash_table_remove(rio_vmi->bp_events_table, GINT_TO_POINTER(bp_vaddr));
            if (FALSE == ret)
            {
                eprintf("%s: Fail to remove key from breakpoint table\n", __func__);
                return false;
            }

            bp_handled = true;
            if (!b->hw)
            {
                // software breakpoint
                // r2 has to write back the original instruction
                bp_handled = false;
            }
        }
        else
        {
            eprintf("%s: Breakpoint already disabled\n", __func__);
            bp_handled = true;
        }
    }

    return bp_handled;
}

// "drp" register profile
static const char *__reg_profile(RDebug *dbg) {
    eprintf("%s\n", __func__);
    int arch = r_sys_arch_id (dbg->arch);
    int bits = dbg->anal->bits;

    switch (arch) {
    case R_SYS_ARCH_X86:
        switch (bits) {
        case 32:
            return strdup (
            #include "x86-32.h"
                        );
            break;
        case 64:
            return strdup (
            #include "x86-64.h"
                        );
            break;
        default:
            eprintf("bit size not supported by vmi debugger\n");
            return NULL;

        }
        break;
    default:
        eprintf("Architecture not supported by vmi debugger\n");
        return NULL;
    }

}

// "dk" send signal
static bool __kill(RDebug *dbg, __attribute__((unused)) int pid, __attribute__((unused)) int tid, int sig) {
    RIODesc *desc = NULL;
    RIOVmi *rio_vmi = NULL;
    eprintf("%s, sig: %d\n", __func__, sig);

    desc = dbg->iob.io->desc;
    rio_vmi = desc->data;
    if (!rio_vmi)
    {
        eprintf("%s: Invalid RIOVmi\n", __func__);
        return false;
    }

    if (sig < 0 || sig > 31)
        return false;
    return true;
}

static int __select(__attribute__((unused)) int pid, __attribute__((unused)) int tid) {
    eprintf("%s\n", __func__);

    return 1;
}

static RDebugInfo* __info(__attribute__((unused)) RDebug *dbg, __attribute__((unused)) const char *arg) {
    eprintf("%s\n", __func__);

    return NULL;
}

static RList* __frames(__attribute__((unused)) RDebug *dbg, __attribute__((unused)) ut64 at) {
    eprintf("%s\n", __func__);

    return NULL;
}

static int __reg_read(RDebug *dbg, int type, ut8 *buf, int size) {
    RIODesc *desc = NULL;
    RIOVmi *rio_vmi = NULL;
    status_t status = 0;
    int buf_size = 0;
    pid_t pid;
    uint64_t cr3 = 0;
    registers_t regs;

    eprintf("%s, type: %d, size:%d\n", __func__, type, size);

    desc = dbg->iob.io->desc;
    rio_vmi = desc->data;
    if (!rio_vmi)
    {
        eprintf("%s: Invalid RIOVmi\n", __func__);
        return 1;
    }

    if (!rio_vmi->attached)
        return 0;

    unsigned int nb_vcpus = vmi_get_num_vcpus(rio_vmi->vmi);

    bool found = false;
    for (unsigned int vcpu = 0; vcpu < nb_vcpus; vcpu++)
    {
        // get cr3
        // if we have just attached, we cannot rely on vcpu_reg() since the VCPU
        // state has not been synchronized with the new CR3 value from the attach event
        if (rio_vmi->pid_cr3)
            cr3 = rio_vmi->pid_cr3;
        else
        {
            // TODO: never reached, pid_cr3 is set since cb_on_cr3_load
            // and is always valid
            status = vmi_get_vcpureg(rio_vmi->vmi, &cr3, CR3, vcpu);
            if (status == VMI_FAILURE)
            {
                eprintf("Fail to get vcpu registers\n");
                return 1;
            }
        }
        // convert to pid
        status = vmi_dtb_to_pid(rio_vmi->vmi, cr3, &pid);
        if (status == VMI_FAILURE)
        {
            eprintf("Fail to convert CR3 to PID\n");
            return 1;
        }
        if (pid == rio_vmi->pid)
        {
            found = true;

            // get registers
            status = vmi_get_vcpuregs(rio_vmi->vmi, &regs, vcpu);
            if (status == VMI_FAILURE)
            {
                eprintf("Fail to get vcpu registers\n");
                return 1;
            }
            break;
        }
    }
    if (!found)
    {
        eprintf("Cannot find CR3 !\n");
        return 1;
    }

    int arch = r_sys_arch_id (dbg->arch);
    int bits = dbg->anal->bits;

    switch (arch) {
    case R_SYS_ARCH_X86:
        switch (bits) {
        case 32:
            memcpy(buf      , &(regs.x86.rax), sizeof(uint32_t));
            memcpy(buf + 4  , &(regs.x86.rcx), sizeof(uint32_t));
            memcpy(buf + 8 , &(regs.x86.rdx), sizeof(uint32_t));
            memcpy(buf + 12 , &(regs.x86.rbx), sizeof(uint32_t));
            memcpy(buf + 16 , &(regs.x86.rsp), sizeof(uint32_t));
            memcpy(buf + 20 , &(regs.x86.rbp), sizeof(uint32_t));
            memcpy(buf + 24 , &(regs.x86.rsi), sizeof(uint32_t));
            memcpy(buf + 28 , &(regs.x86.rdi), sizeof(uint32_t));
            memcpy(buf + 32 , &(regs.x86.rip), sizeof(uint32_t));
            break;
        case 64:
            memcpy(buf      , &(regs.x86.rax), sizeof(regs.x86.rax));
            memcpy(buf + 8  , &(regs.x86.rbx), sizeof(regs.x86.rbx));
            memcpy(buf + 16 , &(regs.x86.rcx), sizeof(regs.x86.rcx));
            memcpy(buf + 24 , &(regs.x86.rdx), sizeof(regs.x86.rdx));
            memcpy(buf + 32 , &(regs.x86.rsi), sizeof(regs.x86.rsi));
            memcpy(buf + 40 , &(regs.x86.rdi), sizeof(regs.x86.rdi));
            memcpy(buf + 48 , &(regs.x86.rbp), sizeof(regs.x86.rbp));
            memcpy(buf + 56 , &(regs.x86.rsp), sizeof(regs.x86.rsp));
            memcpy(buf + 64 , &(regs.x86.r8), sizeof(regs.x86.r8));
            memcpy(buf + 72 , &(regs.x86.r9), sizeof(regs.x86.r9));
            memcpy(buf + 80 , &(regs.x86.r10), sizeof(regs.x86.r10));
            memcpy(buf + 88 , &(regs.x86.r11), sizeof(regs.x86.r11));
            memcpy(buf + 96 , &(regs.x86.r12), sizeof(regs.x86.r12));
            memcpy(buf + 104 , &(regs.x86.r13), sizeof(regs.x86.r13));
            memcpy(buf + 112 , &(regs.x86.r14), sizeof(regs.x86.r14));
            memcpy(buf + 120 , &(regs.x86.r15), sizeof(regs.x86.r15));
            memcpy(buf + 128, &(regs.x86.rip), sizeof(regs.x86.rip));
            break;
        }
        break;
    default:
        eprintf("Architecture not supported\n");
        return 1;
    }
    buf_size = 128 + sizeof(uint64_t);

    return buf_size;
}

RDebugPlugin r_debug_plugin_vmi = {
    .name = "vmi",
    .license = "LGPL3",
    .arch = "x86",
    .bits = R_SYS_BITS_32 | R_SYS_BITS_64,
    .canstep = 1,
    .info = &__info,
    .attach = &__attach,
    .detach = &__detach,
    .select = &__select,
    .threads = &__threads,
    .step = &__step,
    .cont = &__continue,
    .wait = &__wait,
    .kill = &__kill,
    .frames = &__frames,
    .reg_read = &__reg_read,
    .reg_profile = (void*) &__reg_profile,
    .map_get = &__map_get,
    .modules_get = &__modules_get,
    .breakpoint = &__breakpoint,
};

#ifndef CORELIB
RLibStruct radare_plugin = {
    .type = R_LIB_TYPE_DBG,
    .data = &r_debug_plugin_vmi,
    .version = R2_VERSION
};
#endif
