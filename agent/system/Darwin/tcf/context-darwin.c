/*******************************************************************************
 * Copyright (c) 2007, 2013 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 * You may elect to redistribute this code under either of these licenses.
 *
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * This module handles process/thread OS contexts and their state machine.
 */

#include <tcf/config.h>

#if defined(__APPLE__)

#if ENABLE_DebugContext && !ENABLE_ContextProxy

#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/ptrace.h>
#include <mach/thread_status.h>
#include <tcf/framework/context.h>
#include <tcf/framework/events.h>
#include <tcf/framework/errors.h>
#include <tcf/framework/trace.h>
#include <tcf/framework/myalloc.h>
#include <tcf/framework/waitpid.h>
#include <tcf/framework/signames.h>
#include <tcf/services/symbols.h>
#include <tcf/services/breakpoints.h>
#include <system/Darwin/tcf/regset.h>

#define WORD_SIZE   4

typedef struct ContextExtensionDarwin {
    pid_t                   pid;
    ContextAttachCallBack * attach_callback;
    void *                  attach_data;
    int                     ptrace_flags;
    int                     ptrace_event;
    int                     syscall_enter;
    int                     syscall_exit;
    int                     syscall_id;
    ContextAddress          syscall_pc;
    ContextAddress          loader_state;
    int                     end_of_step;
    REG_SET *               regs;               /* copy of context registers, updated when context stops */
    ErrorReport *           regs_error;         /* if not NULL, 'regs' is invalid */
    int                     regs_dirty;         /* if not 0, 'regs' is modified and needs to be saved before context is continued */
    int                     pending_step;
} ContextExtensionDarwin;

static size_t context_extension_offset = 0;

#define EXT(ctx) ((ContextExtensionDarwin *)((char *)(ctx) + context_extension_offset))

#include <tcf/framework/pid-hash.h>

static LINK pending_list = TCF_LIST_INIT(pending_list);

static MemoryErrorInfo mem_err_info;

const char * context_suspend_reason(Context * ctx) {
    static char reason[128];

    if (EXT(ctx)->end_of_step) return REASON_STEP;
    if (EXT(ctx)->syscall_enter) return "System Call";
    if (EXT(ctx)->syscall_exit) return "System Return";
    if (ctx->signal == SIGSTOP || ctx->signal == SIGTRAP) return REASON_USER_REQUEST;
    snprintf(reason, sizeof(reason), "Signal %d", ctx->signal);
    return reason;
}

int context_attach_self(void) {
    if (ptrace(PT_TRACE_ME, 0, 0, 0) < 0) {
        int err = errno;
        trace(LOG_ALWAYS, "error: ptrace(PT_TRACE_ME) failed: pid %d, error %d %s",
              getpid(), err, errno_to_str(err));
        errno = err;
        return -1;
    }
    return 0;
}

int context_attach(pid_t pid, ContextAttachCallBack * done, void * data, int mode) {
    Context * ctx = NULL;

    assert(done != NULL);
    trace(LOG_CONTEXT, "context: attaching pid %d", pid);
    if ((mode & CONTEXT_ATTACH_SELF) == 0 && ptrace(PT_ATTACH, pid, 0, 0) < 0) {
        int err = errno;
        trace(LOG_ALWAYS, "error: ptrace(PT_ATTACH) failed: pid %d, error %d %s",
            pid, err, errno_to_str(err));
        errno = err;
        return -1;
    }
    add_waitpid_process(pid);
    ctx = create_context(pid2id(pid, 0));
    ctx->mem = ctx;
    ctx->mem_access |= MEM_ACCESS_INSTRUCTION;
    ctx->mem_access |= MEM_ACCESS_DATA;
    ctx->mem_access |= MEM_ACCESS_USER;
    ctx->big_endian = big_endian_host();
    EXT(ctx)->pid = pid;
    EXT(ctx)->attach_callback = done;
    EXT(ctx)->attach_data = data;
    list_add_first(&ctx->ctxl, &pending_list);
    /* TODO: context_attach works only for main task in a process */
    return 0;
}

int context_has_state(Context * ctx) {
    return ctx != NULL && ctx->parent != NULL;
}

int context_stop(Context * ctx) {
    trace(LOG_CONTEXT, "context:%s suspending ctx %#lx, id %s",
        ctx->pending_intercept ? "" : " temporary", ctx, ctx->id);
    assert(is_dispatch_thread());
    assert(!ctx->exited);
    assert(!ctx->stopped);
    assert(!EXT(ctx)->regs_dirty);
    if (kill(EXT(ctx)->pid, SIGSTOP) < 0) {
        int err = errno;
        if (err == ESRCH) {
            ctx->exiting = 1;
            return 0;
        }
        trace(LOG_ALWAYS, "error: tkill(SIGSTOP) failed: ctx %#lx, id %s, error %d %s",
            ctx, ctx->id, err, errno_to_str(err));
        errno = err;
        return -1;
    }
    return 0;
}

static int syscall_never_returns(Context * ctx) {
    if (EXT(ctx)->syscall_enter) {
        switch (EXT(ctx)->syscall_id) {
        case SYS_sigreturn:
            return 1;
        }
    }
    return 0;
}

int context_continue(Context * ctx) {
    int signal = 0;

    assert(is_dispatch_thread());
    assert(ctx->stopped);
    assert(!ctx->exited);
    assert(!ctx->pending_intercept);
    assert(!EXT(ctx)->pending_step);

    if (skip_breakpoint(ctx, 0)) return 0;

    if (!EXT(ctx)->syscall_enter) {
        unsigned n = 0;
        while (sigset_get_next(&ctx->pending_signals, &n)) {
            if (sigset_get(&ctx->sig_dont_pass, n)) {
                sigset_set(&ctx->pending_signals, n, 0);
            }
            else {
                signal = n;
                break;
            }
        }
        assert(signal != SIGSTOP);
        assert(signal != SIGTRAP);
    }

    trace(LOG_CONTEXT, "context: resuming ctx %#lx, id %s, with signal %d", ctx, ctx->id, signal);
#if defined(__i386__)
    if (EXT(ctx)->regs->__eflags & 0x100) {
        EXT(ctx)->regs->__eflags &= ~0x100;
        EXT(ctx)->regs_dirty = 1;
    }
#elif defined(__x86_64__)
    if (EXT(ctx)->regs->__rflags & 0x100) {
        EXT(ctx)->regs->__rflags &= ~0x100;
        EXT(ctx)->regs_dirty = 1;
    }
#endif
    if (EXT(ctx)->regs_dirty) {
        unsigned int state_count;
        if (thread_set_state(EXT(ctx)->pid, x86_THREAD_STATE32, EXT(ctx)->regs, &state_count) != KERN_SUCCESS) {
            int err = errno;
            trace(LOG_ALWAYS, "error: thread_set_state failed: ctx %#lx, id %s, error %d %s",
                ctx, ctx->id, err, errno_to_str(err));
            errno = err;
            return -1;
        }
        EXT(ctx)->regs_dirty = 0;
    }
    if (ptrace(PT_CONTINUE, EXT(ctx)->pid, 0, signal) < 0) {
        int err = errno;
        if (err == ESRCH) {
            send_context_started_event(ctx);
            return 0;
        }
        trace(LOG_ALWAYS, "error: ptrace(PT_CONTINUE, ...) failed: ctx %#lx, id %s, error %d %s",
            ctx, ctx->id, err, errno_to_str(err));
        errno = err;
        return -1;
    }
    sigset_set(&ctx->pending_signals, signal, 0);
    if (syscall_never_returns(ctx)) {
        EXT(ctx)->syscall_enter = 0;
        EXT(ctx)->syscall_exit = 0;
        EXT(ctx)->syscall_id = 0;
    }
    send_context_started_event(ctx);
    return 0;
}

int context_single_step(Context * ctx) {
    assert(is_dispatch_thread());
    assert(context_has_state(ctx));
    assert(ctx->stopped);
    assert(!ctx->exited);
    assert(!EXT(ctx)->pending_step);

    if (skip_breakpoint(ctx, 1)) return 0;

    if (syscall_never_returns(ctx)) return context_continue(ctx);
    trace(LOG_CONTEXT, "context: single step ctx %#lx, id %S", ctx, ctx->id);
    if (EXT(ctx)->regs_dirty) {
        unsigned int state_count;
        if (thread_set_state(EXT(ctx)->pid, x86_THREAD_STATE32, EXT(ctx)->regs, &state_count) != KERN_SUCCESS) {
            int err = errno;
            trace(LOG_ALWAYS, "error: thread_set_state failed: ctx %#lx, id %s, error %d %s",
                ctx, ctx->id, err, errno_to_str(err));
            errno = err;
            return -1;
        }
        EXT(ctx)->regs_dirty = 0;
    }
    if (ptrace(PT_STEP, EXT(ctx)->pid, 0, 0) < 0) {
        int err = errno;
        if (err == ESRCH) {
            EXT(ctx)->pending_step = 1;
            send_context_started_event(ctx);
            return 0;
        }
        trace(LOG_ALWAYS, "error: ptrace(PT_STEP, ...) failed: ctx %#lx, id %s, error %d %s",
            ctx, ctx->id, err, errno_to_str(err));
        errno = err;
        return -1;
    }
    EXT(ctx)->pending_step = 1;
    send_context_started_event(ctx);
    return 0;
}

int context_resume(Context * ctx, int mode, ContextAddress range_start, ContextAddress range_end) {
    switch (mode) {
    case RM_RESUME:
        return context_continue(ctx);
    case RM_STEP_INTO:
        return context_single_step(ctx);
    case RM_TERMINATE:
        sigset_set(&ctx->pending_signals, SIGKILL, 1);
        return context_continue(ctx);
    }
    errno = ERR_UNSUPPORTED;
    return -1;
}

int context_can_resume(Context * ctx, int mode) {
    switch (mode) {
    case RM_RESUME:
        return 1;
    case RM_STEP_INTO:
    case RM_TERMINATE:
        return context_has_state(ctx);
    }
    return 0;
}

int context_write_mem(Context * ctx, ContextAddress address, void * buf, size_t size) {
    /*
    ContextAddress word_addr;
    assert(is_dispatch_thread());
    assert(!ctx->exited);
    trace(LOG_CONTEXT, "context: write memory ctx %#lx, id %s, address %#lx, size %zu",
        ctx, ctx->id, address, size);
    assert(WORD_SIZE == sizeof(unsigned));
    check_breakpoints_on_memory_write(ctx, address, buf, size);
    for (word_addr = address & ~(WORD_SIZE - 1); word_addr < address + size; word_addr += WORD_SIZE) {
        unsigned word = 0;
        if (word_addr < address || word_addr + WORD_SIZE > address + size) {
            int i;
            errno = 0;
            word = ptrace(PT_PEEKDATA, EXT(ctx)->pid, word_addr, 0);
            if (errno != 0) {
                int err = errno;
                trace(LOG_CONTEXT, "error: ptrace(PT_PEEKDATA, ...) failed: ctx %#lx, id %s, error %d %s",
                    ctx, ctx->id, err, errno_to_str(err));
                errno = err;
                return -1;
            }
            for (i = 0; i < WORD_SIZE; i++) {
                if (word_addr + i >= address && word_addr + i < address + size) {
                    ((char *)&word)[i] = ((char *)buf)[word_addr + i - address];
                }
            }
        }
        else {
            word = *(unsigned *)((char *)buf + (word_addr - address));
        }
        if (ptrace(PT_POKEDATA, EXT(ctx)->pid, word_addr, word) < 0) {
            int err = errno;
            trace(LOG_ALWAYS, "error: ptrace(PT_POKEDATA, ...) failed: ctx %#lx, id %s, error %d %s",
                ctx, ctx->id, err, errno_to_str(err));
            errno = err;
            return -1;
        }
    }
    */
    return 0;
}

int context_read_mem(Context * ctx, ContextAddress address, void * buf, size_t size) {
    /*
    ContextAddress word_addr;
    assert(is_dispatch_thread());
    assert(!ctx->exited);
    trace(LOG_CONTEXT, "context: read memory ctx %#lx, id %s, address %#lx, size %zu",
        ctx, ctx->id, address, size);
    assert(WORD_SIZE == sizeof(unsigned));
    for (word_addr = address & ~(WORD_SIZE - 1); word_addr < address + size; word_addr += WORD_SIZE) {
        unsigned word = 0;
        errno = 0;
        word = ptrace(PT_PEEKDATA, EXT(ctx)->pid, word_addr, 0);
        if (errno != 0) {
            int err = errno;
            trace(LOG_CONTEXT, "error: ptrace(PT_PEEKDATA, ...) failed: ctx %#lx, id %s, error %d %s",
                ctx, ctx->id, err, errno_to_str(err));
            errno = err;
            return -1;
        }
        if (word_addr < address || word_addr + WORD_SIZE > address + size) {
            int i;
            for (i = 0; i < WORD_SIZE; i++) {
                if (word_addr + i >= address && word_addr + i < address + size) {
                    ((char *)buf)[word_addr + i - address] = ((char *)&word)[i];
                }
            }
        }
        else {
            *(unsigned *)((char *)buf + (word_addr - address)) = word;
        }
    }
    check_breakpoints_on_memory_read(ctx, address, buf, size);
    */
    return 0;
}

#if ENABLE_ExtendedMemoryErrorReports
int context_get_mem_error_info(MemoryErrorInfo * info) {
    if (mem_err_info.error == 0) {
        set_errno(ERR_OTHER, "Extended memory error info not available");
        return -1;
    }
    *info = mem_err_info;
    return 0;
}
#endif

int context_write_reg(Context * ctx, RegisterDefinition * def, unsigned offs, unsigned size, void * buf) {
    ContextExtensionDarwin * ext = EXT(ctx);

    assert(is_dispatch_thread());
    assert(context_has_state(ctx));
    assert(ctx->stopped);
    assert(!ctx->exited);
    assert(offs + size <= def->size);

    if (ext->regs_error) {
        set_error_report_errno(ext->regs_error);
        return -1;
    }
    memcpy((uint8_t *)ext->regs + def->offset + offs, buf, size);
    ext->regs_dirty = 1;
    return 0;
}

int context_read_reg(Context * ctx, RegisterDefinition * def, unsigned offs, unsigned size, void * buf) {
    ContextExtensionDarwin * ext = EXT(ctx);

    assert(is_dispatch_thread());
    assert(context_has_state(ctx));
    assert(ctx->stopped);
    assert(!ctx->exited);
    assert(offs + size <= def->size);

    if (ext->regs_error) {
        set_error_report_errno(ext->regs_error);
        return -1;
    }
    memcpy(buf, (uint8_t *)ext->regs + def->offset + offs, size);
    return 0;
}

unsigned context_word_size(Context * ctx) {
    return sizeof(void *);
}

int context_get_canonical_addr(Context * ctx, ContextAddress addr,
        Context ** canonical_ctx, ContextAddress * canonical_addr,
        ContextAddress * block_addr, ContextAddress * block_size) {
    /* Direct mapping, page size is irrelevant */
    ContextAddress page_size = 0x100000;
    assert(is_dispatch_thread());
    *canonical_ctx = ctx->mem;
    if (canonical_addr != NULL) *canonical_addr = addr;
    if (block_addr != NULL) *block_addr = addr & ~(page_size - 1);
    if (block_size != NULL) *block_size = page_size;
    return 0;
}

Context * context_get_group(Context * ctx, int group) {
    static Context * cpu_group = NULL;
    switch (group) {
    case CONTEXT_GROUP_INTERCEPT:
        return ctx;
    case CONTEXT_GROUP_CPU:
        if (cpu_group == NULL) cpu_group = create_context("CPU");
        return cpu_group;
    }
    return ctx->mem;
}

int context_get_supported_bp_access_types(Context * ctx) {
    return 0;
}

int context_plant_breakpoint(ContextBreakpoint * bp) {
    errno = ERR_UNSUPPORTED;
    return -1;
}

int context_unplant_breakpoint(ContextBreakpoint * bp) {
    errno = ERR_UNSUPPORTED;
    return -1;
}

int context_get_memory_map(Context * ctx, MemoryMap * map) {
    ctx = ctx->mem;
    assert(!ctx->exited);
    return 0;
}

#if ENABLE_ContextISA
int context_get_isa(Context * ctx, ContextAddress addr, ContextISA * isa) {
    memset(isa, 0, sizeof(ContextISA));
#if defined(__i386__)
    isa->def = "386";
#elif defined(__x86_64__)
    isa->def = "X86_64";
#else
    isa->def = NULL;
#endif
#if SERVICE_Symbols
    if (get_context_isa(ctx, addr, &isa->isa, &isa->addr, &isa->size) < 0) return -1;
#endif
    return 0;
}
#endif

static Context * find_pending(pid_t pid) {
    LINK * l = pending_list.next;
    while (l != &pending_list) {
        Context * c = ctxl2ctxp(l);
        if (EXT(c)->pid == pid) {
            list_remove(&c->ctxl);
            return c;
        }
        l = l->next;
    }
    return NULL;
}

static void event_pid_exited(pid_t pid, int status, int signal) {
    Context * ctx;

    ctx = context_find_from_pid(pid, 1);
    if (ctx == NULL) {
        ctx = find_pending(pid);
        if (ctx == NULL) {
            trace(LOG_EVENTS, "event: ctx not found, pid %d, exit status %d, term signal %d", pid, status, signal);
        }
        else {
            assert(ctx->ref_count == 0);
            ctx->ref_count = 1;
            if (EXT(ctx)->attach_callback != NULL) {
                if (status == 0) status = EINVAL;
                EXT(ctx)->attach_callback(status, ctx, EXT(ctx)->attach_data);
            }
            assert(list_is_empty(&ctx->children));
            assert(ctx->parent == NULL);
            ctx->exited = 1;
            context_unlock(ctx);
        }
    }
    else {
        /* Note: ctx->exiting should be 1 here. However, PTRACE_EVENT_EXIT can be lost by PTRACE because of racing
         * between PTRACE_CONT (or PTRACE_SYSCALL) and SIGTRAP/PTRACE_EVENT_EXIT. So, ctx->exiting can be 0.
         */
        if (EXT(ctx->parent)->pid == pid) ctx = ctx->parent;
        assert(EXT(ctx)->attach_callback == NULL);
        if (ctx->exited) {
            trace(LOG_EVENTS, "event: ctx %#lx, pid %d, exit status %d unexpected, stopped %d, exited %d",
                ctx, pid, status, ctx->stopped, ctx->exited);
        }
        else {
            trace(LOG_EVENTS, "event: ctx %#lx, pid %d, exit status %d, term signal %d", ctx, pid, status, signal);
            ctx->exiting = 1;
            if (ctx->stopped) send_context_started_event(ctx);
            if (!list_is_empty(&ctx->children)) {
                LINK * l = ctx->children.next;
                while (l != &ctx->children) {
                    Context * c = cldl2ctxp(l);
                    l = l->next;
                    assert(c->parent == ctx);
                    if (!c->exited) {
                        c->exiting = 1;
                        if (c->stopped) send_context_started_event(c);
                        release_error_report(EXT(c)->regs_error);
                        loc_free(EXT(c)->regs);
                        EXT(c)->regs_error = NULL;
                        EXT(c)->regs = NULL;
                        send_context_exited_event(c);
                    }
                }
            }
            release_error_report(EXT(ctx)->regs_error);
            loc_free(EXT(ctx)->regs);
            EXT(ctx)->regs_error = NULL;
            EXT(ctx)->regs = NULL;
            send_context_exited_event(ctx);
        }
    }
}

static void event_pid_stopped(pid_t pid, int signal, int event, int syscall) {
    int stopped_by_exception = 0;
    unsigned long msg = 0;
    Context * ctx = NULL;
    Context * ctx2 = NULL;

    trace(LOG_EVENTS, "event: pid %d stopped, signal %d", pid, signal);

    ctx = context_find_from_pid(pid, 1);

    if (ctx == NULL) {
        ctx = find_pending(pid);
        if (ctx != NULL) {
            Context * prs = ctx;
            assert(prs->ref_count == 0);
            ctx = create_context(pid2id(pid, pid));
            EXT(ctx)->pid = pid;
            EXT(ctx)->regs = (REG_SET *)loc_alloc(sizeof(REG_SET));
            ctx->pending_intercept = 1;
            ctx->mem = prs;
            ctx->parent = prs;
            ctx->big_endian = prs->big_endian;
            prs->ref_count++;
            list_add_last(&ctx->cldl, &prs->children);
            link_context(prs);
            link_context(ctx);
            send_context_created_event(prs);
            send_context_created_event(ctx);
            if (EXT(prs)->attach_callback) {
                EXT(prs)->attach_callback(0, prs, EXT(prs)->attach_data);
                EXT(prs)->attach_callback = NULL;
                EXT(prs)->attach_data = NULL;
            }
        }
    }

    if (ctx == NULL) return;

    assert(!ctx->exited);
    assert(!EXT(ctx)->attach_callback);

    if (signal != SIGSTOP && signal != SIGTRAP) {
        sigset_set(&ctx->pending_signals, signal, 1);
        if (sigset_get(&ctx->sig_dont_stop, signal) == 0) {
            ctx->pending_intercept = 1;
            stopped_by_exception = 1;
        }
    }

    if (ctx->stopped) {
        send_context_changed_event(ctx);
    }
    else {
        thread_state_t state;
        unsigned int state_count;
        ContextAddress pc0 = 0;
        ContextAddress pc1 = 0;

        assert(!EXT(ctx)->regs_dirty);

        EXT(ctx)->end_of_step = 0;
        EXT(ctx)->ptrace_event = event;
        ctx->signal = signal;
        ctx->stopped_by_bp = 0;
        ctx->stopped_by_exception = stopped_by_exception;
        ctx->stopped = 1;

        if (EXT(ctx)->regs_error) {
            release_error_report(EXT(ctx)->regs_error);
            EXT(ctx)->regs_error = NULL;
        }
        else {
            pc0 = get_regs_PC(ctx);
        }

        if (thread_get_state(EXT(ctx)->pid, x86_THREAD_STATE32, EXT(ctx)->regs, &state_count) != KERN_SUCCESS) {
            assert(errno != 0);
            EXT(ctx)->regs_error = get_error_report(errno);
            trace(LOG_ALWAYS, "error: thread_get_state failed; id %s, error %d %s",
                ctx->id, errno, errno_to_str(errno));
        }
        else {
            pc1 = get_regs_PC(ctx);
        }

        if (!EXT(ctx)->syscall_enter || EXT(ctx)->regs_error || pc0 != pc1) {
            EXT(ctx)->syscall_enter = 0;
            EXT(ctx)->syscall_exit = 0;
            EXT(ctx)->syscall_id = 0;
            EXT(ctx)->syscall_pc = 0;
        }
        trace(LOG_EVENTS, "event: pid %d stopped at PC = %#lx", pid, pc1);

        if (signal == SIGTRAP && event == 0 && !syscall) {
            size_t break_size = 0;
            get_break_instruction(ctx, &break_size);
            ctx->stopped_by_bp = !EXT(ctx)->regs_error && is_breakpoint_address(ctx, pc1 - break_size);
            EXT(ctx)->end_of_step = !ctx->stopped_by_bp && EXT(ctx)->pending_step;
            if (ctx->stopped_by_bp) set_regs_PC(ctx, pc1 - break_size);
        }
        EXT(ctx)->pending_step = 0;
        send_context_stopped_event(ctx);
    }
}

static void waitpid_listener(int pid, int exited, int exit_code, int signal, int event_code, int syscall, void * args) {
    if (exited) {
        event_pid_exited(pid, exit_code, signal);
    }
    else {
        event_pid_stopped(pid, signal, event_code, syscall);
    }
}

void init_contexts_sys_dep(void) {
    context_extension_offset = context_extension(sizeof(ContextExtensionDarwin));
    add_waitpid_listener(waitpid_listener, NULL);
    ini_context_pid_hash();
}

#endif  /* if ENABLE_DebugContext */
#endif /* __APPLE__ */
