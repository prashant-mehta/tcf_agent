/*******************************************************************************
 * Copyright (c) 2007, 2008 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials 
 * are made available under the terms of the Eclipse Public License v1.0 
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * The Eclipse Public License is available at
 * http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at 
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *  
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * This module implements Breakpoints service.
 * The service maintains a list of breakpoints.
 * Each breakpoint consists of one or more conditions that determine
 * when a program's execution should be interrupted.
 */

#include "mdep.h"
#include "config.h"
#if SERVICE_Breakpoints

/* TODO: replant breakpoints when shared lib is loaded or unloaded */

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include "breakpoints.h"
#include "expressions.h"
#include "channel.h"
#include "protocol.h"
#include "errors.h"
#include "trace.h"
#include "runctrl.h"
#include "context.h"
#include "myalloc.h"
#include "exceptions.h"
#include "symbols.h"
#include "json.h"
#include "link.h"
#include "linenumbers.h"
#include "stacktrace.h"

#if defined(_WRS_KERNEL)
#  include <private/vxdbgLibP.h>
#endif

typedef struct BreakpointRef BreakpointRef;
typedef struct BreakpointAttribute BreakpointAttribute;
typedef struct BreakpointInfo BreakpointInfo;
typedef struct BreakInstruction BreakInstruction;

struct BreakpointRef {
    LINK link_inp;
    LINK link_bp;
    InputStream * inp;
    BreakpointInfo * bp;
};

struct BreakpointAttribute {
    BreakpointAttribute * next;
    char * name;
    char * value;
};

struct BreakpointInfo {
    LINK link_all;
    LINK link_id;
    LINK refs;
    char id[64];
    int enabled;
    int planted;
    int deleted;
    int error;
    char * err_msg;
    char * address;
    char * condition;
#if SERVICE_LineNumbers
    char * file;
    int line;
    int column;
#endif
    int ignore_count;
    int hit_count;
    BreakpointAttribute * unsupported;

    /* Last status report contents: */
    int status_unsupported;
    int status_error;
    int status_planted;
};

struct BreakInstruction {
    LINK link_all;
    LINK link_adr;
    Context * ctx;
    int ctx_cnt;
    ContextAddress address;
#if defined(_WRS_KERNEL)
    VXDBG_CTX vxdbg_ctx;
    VXDBG_BP_ID vxdbg_id;
#else
    char saved_code[BREAK_SIZE];
#endif    
    int error;
    int skip;
    BreakpointInfo ** refs;
    int ref_size;
    int ref_cnt;
    int planted;
};

static const char * BREAKPOINTS = "Breakpoints";

#define is_running(ctx) (!(ctx)->stopped && context_has_state(ctx))

#define ADDR2INSTR_HASH_SIZE 1023
#define addr2instr_hash(addr) ((unsigned)((addr) + ((addr) >> 8)) % ADDR2INSTR_HASH_SIZE)

#define link_all2bi(A)  ((BreakInstruction *)((char *)(A) - (int)&((BreakInstruction *)0)->link_all))
#define link_adr2bi(A)  ((BreakInstruction *)((char *)(A) - (int)&((BreakInstruction *)0)->link_adr))

#define ID2BP_HASH_SIZE 1023

#define link_all2bp(A)  ((BreakpointInfo *)((char *)(A) - (int)&((BreakpointInfo *)0)->link_all))
#define link_id2bp(A)   ((BreakpointInfo *)((char *)(A) - (int)&((BreakpointInfo *)0)->link_id))

#define INP2BR_HASH_SIZE 127

#define link_inp2br(A)  ((BreakpointRef *)((char *)(A) - (int)&((BreakpointRef *)0)->link_inp))
#define link_bp2br(A)   ((BreakpointRef *)((char *)(A) - (int)&((BreakpointRef *)0)->link_bp))

static LINK breakpoints;
static LINK id2bp[ID2BP_HASH_SIZE];

static LINK instructions;
static LINK addr2instr[ADDR2INSTR_HASH_SIZE];

static LINK inp2br[INP2BR_HASH_SIZE];

static int replanting = 0;

static unsigned id2bp_hash(char * id) {
    unsigned hash = 0;
    while (*id) hash = (hash >> 16) + hash + (unsigned char)*id++;
    return hash % ID2BP_HASH_SIZE;
}

static void plant_instruction(BreakInstruction * bi) {
    assert(!bi->skip);
    assert(!bi->planted);
    bi->error = 0;
#if defined(_WRS_KERNEL)
    bi->vxdbg_ctx.ctxId = bi->ctx_cnt == 1 ? bi->ctx->pid : 0;
    bi->vxdbg_ctx.ctxId = 0;
    bi->vxdbg_ctx.ctxType = VXDBG_CTX_TASK;
    if (vxdbgBpAdd(vxdbg_clnt_id,
            &bi->vxdbg_ctx, 0, BP_ACTION_STOP | BP_ACTION_NOTIFY,
            0, 0, (INSTR *)bi->address, 0, 0, &bi->vxdbg_id) != OK) {
        bi->error = errno;
        assert(bi->error != 0);
    }
#else
    if (context_read_mem(bi->ctx, bi->address, bi->saved_code, BREAK_SIZE) < 0) {
        bi->error = errno;
    }
    else if (context_write_mem(bi->ctx, bi->address, &BREAK_INST, BREAK_SIZE) < 0) {
        bi->error = errno;
    }
#endif
    bi->planted = bi->error == 0;
}

static int verify_instruction(BreakInstruction * bi) {
    assert(bi->planted);
#if defined(_WRS_KERNEL)
    return bi->vxdbg_ctx.ctxId == (bi->ctx_cnt == 1 ? bi->ctx->pid : 0) &&
           bi->vxdbg_ctx.ctxType == VXDBG_CTX_TASK;
#else
    return 1;
#endif
}

static void remove_instruction(BreakInstruction * bi) {
    assert(bi->planted);
    assert(!bi->error);
#if defined(_WRS_KERNEL)
    {
        VXDBG_BP_DEL_INFO info;
        memset(&info, 0, sizeof(info));
        info.pClnt = vxdbg_clnt_id;
        info.type = BP_BY_ID_DELETE;
        info.info.id.bpId = bi->vxdbg_id;
        if (vxdbgBpDelete(info) != OK) {
            bi->error = errno;
            assert(bi->error != 0);
        }
    }
#else
    if (!bi->ctx->exited && !is_running(bi->ctx)) {
        if (context_write_mem(bi->ctx, bi->address, bi->saved_code, BREAK_SIZE) < 0) {
            bi->error = errno;
        }
    }
#endif
    bi->planted = 0;
}

static BreakInstruction * add_instruction(Context * ctx, ContextAddress address) {
    int hash = addr2instr_hash(address);
    BreakInstruction * bi = (BreakInstruction *)loc_alloc_zero(sizeof(BreakInstruction));
    list_add_last(&bi->link_all, &instructions);
    list_add_last(&bi->link_adr, addr2instr + hash);
    context_lock(ctx);
    bi->ctx = ctx;
    bi->address = address;
    return bi;
}

static void clear_instruction_refs(void) {
    LINK * l = instructions.next;
    while (l != &instructions) {
        BreakInstruction * bi = link_all2bi(l);
        bi->ctx_cnt = 1;
        bi->ref_cnt = 0;
        l = l->next;
    }
}

static void delete_unused_instructions(void) {
    LINK * l = instructions.next;
    while (l != &instructions) {
        BreakInstruction * bi = link_all2bi(l);
        l = l->next;
        if (bi->skip) continue;
        if (bi->ref_cnt == 0) {
            list_remove(&bi->link_all);
            list_remove(&bi->link_adr);
            if (bi->planted) {
                if (bi->ctx->exited || is_running(bi->ctx)) {
                    LINK * qp = context_root.next;
                    while (qp != &context_root) {
                        Context * ctx = ctxl2ctxp(qp);
                        qp = qp->next;
                        if (ctx->mem == bi->ctx->mem && !ctx->exited && !is_running(ctx)) {
                            assert(bi->ctx != ctx);
                            context_unlock(bi->ctx);
                            context_lock(ctx);
                            bi->ctx = ctx;
                            break;
                        }
                    }
                }
                remove_instruction(bi);
            }
            context_unlock(bi->ctx);
            loc_free(bi->refs);
            loc_free(bi);
        }
        else if (!bi->planted) {
            plant_instruction(bi);
        }
        else if (!verify_instruction(bi)) {
            remove_instruction(bi);
            plant_instruction(bi);
        }
    }
}

static BreakInstruction * find_instruction(Context * ctx, ContextAddress address) {
    int hash = addr2instr_hash(address);
    LINK * l = addr2instr[hash].next;
    assert(!ctx->exited);
    while (l != addr2instr + hash) {
        BreakInstruction * bi = link_adr2bi(l);
        l = l->next;
        if (bi->ctx->mem == ctx->mem && bi->address == address) {
            if (bi->ctx->exited || is_running(bi->ctx)) {
                assert(bi->ctx != ctx);
                context_unlock(bi->ctx);
                context_lock(ctx);
                bi->ctx = ctx;
            }
            return bi;
        }
    }
    return NULL;
}

void check_breakpoints_on_memory_read(Context * ctx, ContextAddress address, void * p, size_t size) {
#if !defined(_WRS_KERNEL)
    int i;
    char * buf = (char *)p;
    LINK * l = instructions.next;
    while (l != &instructions) {
        BreakInstruction * bi = link_all2bi(l);
        l = l->next;
        if (!bi->planted) continue;
        if (bi->ctx->mem != ctx->mem) continue;
        if (bi->address + BREAK_SIZE <= address) continue;
        if (bi->address >= address + size) continue;
        for (i = 0; i < BREAK_SIZE; i++) {
            if (bi->address + i < address) continue;
            if (bi->address + i >= address + size) continue;
            buf[bi->address + i - address] = bi->saved_code[i];
        }
    }
#endif
}

void check_breakpoints_on_memory_write(Context * ctx, ContextAddress address, void * p, size_t size) {
#if !defined(_WRS_KERNEL)
    int i;
    char * buf = (char *)p;
    LINK * l = instructions.next;
    while (l != &instructions) {
        BreakInstruction * bi = link_all2bi(l);
        l = l->next;
        if (!bi->planted) continue;
        if (bi->ctx->mem != ctx->mem) continue;
        if (bi->address + BREAK_SIZE <= address) continue;
        if (bi->address >= address + size) continue;
        for (i = 0; i < BREAK_SIZE; i++) {
            if (bi->address + i < address) continue;
            if (bi->address + i >= address + size) continue;
            bi->saved_code[i] = buf[bi->address + i - address];
            buf[bi->address + i - address] = BREAK_INST[i];
        }
    }
#endif
}

static void write_breakpoint_status(OutputStream * out, BreakpointInfo * bp) {
    BreakpointAttribute * u = bp->unsupported;

    write_stream(out, '{');

    if (u != NULL) {
        char * msg = "Unsupported breakpoint properties: ";
        json_write_string(out, "Error");
        write_stream(out, ':');
        write_stream(out, '"');
        while (*msg) json_write_char(out, *msg++);
        while (u != NULL) {
            msg = u->name;
            while (*msg) json_write_char(out, *msg++);
            u = u->next;
            if (u != NULL) {
                json_write_char(out, ',');
                json_write_char(out, ' ');
            }
        }
        write_stream(out, '"');
    }
    else if (bp->planted) {
        int cnt = 0;
        LINK * l = instructions.next;
        json_write_string(out, "Instances");
        write_stream(out, ':');
        write_stream(out, '[');
        while (l != &instructions) {
            int i = 0;
            BreakInstruction * bi = link_all2bi(l);
            l = l->next;
            while (i < bi->ref_cnt && bi->refs[i] != bp) i++;
            if (i >= bi->ref_cnt) continue;
            if (cnt > 0) write_stream(out, ',');
            write_stream(out, '{');
            json_write_string(out, "LocationContext");
            write_stream(out, ':');
            json_write_string(out, container_id(bi->ctx));
            write_stream(out, ',');
            if (bi->error != 0) {
                json_write_string(out, "Error");
                write_stream(out, ':');
                json_write_string(out, errno_to_str(bi->error));
            }
            else {
                json_write_string(out, "Address");
                write_stream(out, ':');
                json_write_ulong(out, bi->address);
            }
            write_stream(out, '}');
            cnt++;
        }
        write_stream(out, ']');
        assert(cnt > 0);
    }
    else if (bp->error) {
        json_write_string(out, "Error");
        write_stream(out, ':');
        if (bp->err_msg != NULL) {
            json_write_string(out, bp->err_msg);
        }
        else {
            json_write_string(out, errno_to_str(bp->error));
        }
    }

    write_stream(out, '}');
}

static void send_event_breakpoint_status(OutputStream * out, BreakpointInfo * bp) {
    write_stringz(out, "E");
    write_stringz(out, BREAKPOINTS);
    write_stringz(out, "status");

    json_write_string(out, bp->id);
    write_stream(out, 0);
    write_breakpoint_status(out, bp);
    write_stream(out, 0);
    write_stream(out, MARKER_EOM);
}

static void address_expression_error(BreakpointInfo * bp, char * msg) {
    /* TODO: per-context address expression error report */
    int size;
    assert(errno != 0);
    if (bp->error) return;
    bp->error = errno;
    if (msg == NULL) msg = get_expression_error_msg();
    assert(bp->err_msg == NULL);
    size = strlen(msg) + strlen(bp->address) + 64;
    bp->err_msg = loc_alloc(size);
    snprintf(bp->err_msg, size, "Invalid breakpoint address '%s': %s", bp->address, msg);
}

static void plant_breakpoint_in_context(BreakpointInfo * bp, Context * ctx, ContextAddress address) {
    BreakInstruction * bi = NULL;
    bi = find_instruction(ctx, address);
    if (bi == NULL) {
        bi = add_instruction(ctx, address);
    }
    else if (bp->planted) {
        int i = 0;
        while (i < bi->ref_cnt && bi->refs[i] != bp) i++;
        if (i < bi->ref_cnt) return;
    }
    if (bi->ref_cnt >= bi->ref_size) {
        bi->ref_size = bi->ref_size == 0 ? 8 : bi->ref_size * 2;
        bi->refs = (BreakpointInfo **)loc_realloc(bi->refs, sizeof(BreakpointInfo *) * bi->ref_size);
    }
    bi->refs[bi->ref_cnt++] = bp;
    if (bi->ctx != ctx) bi->ctx_cnt++;
    if (bi->error) {
        if (!bp->error) bp->error = bi->error;
    }
    else {
        bp->planted++;
        bp->hit_count = 0;
    }
}

typedef struct PlantBreakpointArgs {
    BreakpointInfo * bp;
    Context * ctx;
} PlantBreakpointArgs;

static void plant_breakpoint_address_iterator(void * x, ContextAddress address) {
    PlantBreakpointArgs * args = (PlantBreakpointArgs *)x;
    plant_breakpoint_in_context(args->bp, args->ctx, address);
}

static void plant_breakpoint(BreakpointInfo * bp) {
    LINK * qp;
    char * p = NULL;
    Value v;
    int context_sensitive_address = 0;

    assert(!bp->planted);
    assert(bp->enabled);
    bp->error = 0;
    if (bp->err_msg != NULL) {
        loc_free(bp->err_msg);
        bp->err_msg = NULL;
    }

    if (bp->address != NULL) {
        if (evaluate_expression(NULL, STACK_NO_FRAME, bp->address, &v) < 0) {
            if (errno != ERR_INV_CONTEXT) {
                address_expression_error(bp, NULL);
                trace(LOG_ALWAYS, "Breakpoints: %s", bp->err_msg);
                return;
            }
            context_sensitive_address = 1;
        }
        if (!context_sensitive_address && v.type != VALUE_INT && v.type != VALUE_UNS) {
            errno = ERR_INV_EXPRESSION;
            address_expression_error(bp, "Must be integer number");
            return;
        }
    }
#if SERVICE_LineNumbers
    else if (bp->file != NULL) {
        context_sensitive_address = 1;
    }
#endif
    else {
        bp->error = ERR_INV_EXPRESSION;
        return;
    }

    for (qp = context_root.next; qp != &context_root; qp = qp->next) {
        Context * ctx = ctxl2ctxp(qp);

        if (ctx->exited || ctx->exiting) continue;
        if (is_running(ctx)) continue;

        if (bp->condition != NULL) {
            /* Optimize away the breakpoint if condition is always false for given context */
            Value c;
            int frame = context_has_state(ctx) ? STACK_TOP_FRAME : STACK_NO_FRAME;
            if (evaluate_expression(ctx, frame, bp->condition, &c) == 0) {
                if (!value_to_boolean(&c)) continue;
            }
        }
        if (context_sensitive_address) {
            if (bp->address != NULL) {
                int frame = context_has_state(ctx) ? STACK_TOP_FRAME : STACK_NO_FRAME;
                if (evaluate_expression(ctx, frame, bp->address, &v) < 0) {
                    address_expression_error(bp, NULL);
                    if (bp->error != ERR_SYM_NOT_FOUND) {
                        trace(LOG_ALWAYS, "Breakpoints: %s", bp->err_msg);
                    }
                    continue;
                }
                if (v.type != VALUE_INT && v.type != VALUE_UNS) {
                    errno = ERR_INV_EXPRESSION;
                    address_expression_error(bp, "Must be integer number");
                    continue;
                }
                plant_breakpoint_in_context(bp, ctx, v.value);
            }
#if SERVICE_LineNumbers
            else if (bp->file != NULL) {
                PlantBreakpointArgs args;
                if (ctx->parent != NULL && ctx->mem == ctx->parent->mem) continue;
                args.ctx = ctx;
                args.bp = bp;
                if (line_to_address(ctx, bp->file, bp->line, bp->column,
                        plant_breakpoint_address_iterator, &args) < 0) {
                    assert(errno != 0);
                    if (bp->error == 0) {
                        bp->error = errno;
                        assert(bp->err_msg == NULL);
                        bp->err_msg = loc_strdup(errno_to_str(bp->error));
                        trace(LOG_ALWAYS, "Breakpoints: %s", bp->err_msg);
                    }
                }
            }
#endif
            else {
                assert(0);
            }
        }
        else {
            if (bp->condition == NULL && ctx->parent != NULL && ctx->mem == ctx->parent->mem) continue;
            plant_breakpoint_in_context(bp, ctx, v.value);
        }
    }
    if (bp->planted) bp->error = 0;
}

static void free_bp(BreakpointInfo * bp) {
    list_remove(&bp->link_all);
    list_remove(&bp->link_id);
    loc_free(bp->err_msg);
    loc_free(bp->address);
#if SERVICE_LineNumbers
    loc_free(bp->file);
#endif
    loc_free(bp->condition);
    while (bp->unsupported != NULL) {
        BreakpointAttribute * u = bp->unsupported;
        bp->unsupported = u->next;
        loc_free(u->name);
        loc_free(u->value);
        loc_free(u);
    }
    assert(list_is_empty(&bp->refs));
    loc_free(bp);
}

static void event_replant_breakpoints(void * arg) {
    int event_cnt = 0;
    TCFBroadcastGroup * bcg = (TCFBroadcastGroup *)arg;
    LINK * l = breakpoints.next;

    replanting = 0;
    clear_instruction_refs();
    while (l != &breakpoints) {
        BreakpointInfo * bp = link_all2bp(l);
        l = l->next;
        if (bp->deleted) {
            free_bp(bp);
            continue;
        }
        bp->planted = 0;
        if (bp->enabled && bp->unsupported == NULL) {
            plant_breakpoint(bp);
        }
        if (bp->status_unsupported != (bp->unsupported != NULL) ||
            bp->status_error != bp->error ||
            bp->status_planted != bp->planted) {
            send_event_breakpoint_status(&bcg->out, bp);
            bp->status_unsupported = bp->unsupported != NULL;
            bp->status_error = bp->error;
            bp->status_planted = bp->planted;
            event_cnt++;
        }
    }
    delete_unused_instructions();
    if (event_cnt > 0) flush_stream(&bcg->out);
}

static void replant_breakpoints(TCFBroadcastGroup * bcg) {
    if (list_is_empty(&breakpoints) && list_is_empty(&instructions)) return;
    if (replanting) return;
    replanting = 1;
    post_safe_event(event_replant_breakpoints, bcg);
}

static int str_equ(char * x, char * y) {
    if (x == y) return 1;
    if (x == NULL) return 0;
    if (y == NULL) return 0;
    return strcmp(x, y) == 0;
}

static int copy_breakpoint_info(BreakpointInfo * dst, BreakpointInfo * src) {
    int res = 0;

    if (strcmp(dst->id, src->id) != 0) {
        strcpy(dst->id, src->id);
        res = 1;
    }

    if (!str_equ(dst->address, src->address)) {
        loc_free(dst->address);
        dst->address = src->address;
        res = 1;
    }
    else {
        loc_free(src->address);
    }
    src->address = NULL;

    if (!str_equ(dst->condition, src->condition)) {
        loc_free(dst->condition);
        dst->condition = src->condition;
        res = 1;
    }
    else {
        loc_free(src->condition);
    }
    src->condition = NULL;

#if SERVICE_LineNumbers
    if (!str_equ(dst->file, src->file)) {
        loc_free(dst->file);
        dst->file = src->file;
        res = 1;
    }
    else {
        loc_free(src->file);
    }
    src->file = NULL;

    if (dst->line != src->line) {
        dst->line = src->line;
        res = 1;
    }

    if (dst->column != src->column) {
        dst->column = src->column;
        res = 1;
    }
#endif

    if (dst->ignore_count != src->ignore_count) {
        dst->ignore_count = src->ignore_count;
        res = 1;
    }

    if (dst->enabled != src->enabled) {
        dst->enabled = src->enabled;
        res = 1;
    }

    if (dst->unsupported != src->unsupported) {
        while (dst->unsupported != NULL) {
            BreakpointAttribute * u = dst->unsupported;
            dst->unsupported = u->next;
            loc_free(u->name);
            loc_free(u->value);
            loc_free(u);
        }
        dst->unsupported = src->unsupported;
        res = 1;
    }
    src->unsupported = NULL;

    return res;
}

static BreakpointInfo * find_breakpoint(char * id) {
    int hash = id2bp_hash(id);
    LINK * l = id2bp[hash].next;
    while (l != id2bp + hash) {
        BreakpointInfo * bp = link_id2bp(l);
        l = l->next;
        if (strcmp(bp->id, id) == 0) return bp;
    }
    return NULL;
}

static BreakpointRef * find_breakpoint_ref(BreakpointInfo * bp, InputStream * inp) {
    LINK * l;
    if (bp == NULL) return NULL;
    l = bp->refs.next;
    while (l != &bp->refs) {
        BreakpointRef * br = link_bp2br(l);
        assert(br->bp == bp);
        if (br->inp == inp) return br;
        l = l->next;
    }
    return NULL;
}

static void read_breakpoint_properties(InputStream * inp, BreakpointInfo * bp) {
    memset(bp, 0, sizeof(BreakpointInfo));
    if (read_stream(inp) != '{') exception(ERR_JSON_SYNTAX);
    if (peek_stream(inp) == '}') {
        read_stream(inp);
    }
    else {
        while (1) {
            int ch;
            char name[256];
            json_read_string(inp, name, sizeof(name));
            if (read_stream(inp) != ':') exception(ERR_JSON_SYNTAX);
            if (strcmp(name, "ID") == 0) {
                json_read_string(inp, bp->id, sizeof(bp->id));
            }
            else if (strcmp(name, "Location") == 0) {
                bp->address = json_read_alloc_string(inp);
            }
            else if (strcmp(name, "Condition") == 0) {
                bp->condition = json_read_alloc_string(inp);
            }
#if SERVICE_LineNumbers
            else if (strcmp(name, "File") == 0) {
                bp->file = json_read_alloc_string(inp);
            }
            else if (strcmp(name, "Line") == 0) {
                bp->line = json_read_long(inp);
            }
            else if (strcmp(name, "Column") == 0) {
                bp->column = json_read_long(inp);
            }
#endif
            else if (strcmp(name, "IgnoreCount") == 0) {
                bp->ignore_count = json_read_long(inp);
            }
            else if (strcmp(name, "Enabled") == 0) {
                bp->enabled = json_read_boolean(inp);
            }
            else {
                BreakpointAttribute * u = (BreakpointAttribute *)loc_alloc(sizeof(BreakpointAttribute));
                u->name = loc_strdup(name);
                u->value = json_skip_object(inp);
                u->next = bp->unsupported;
                bp->unsupported = u;
            }
            ch = read_stream(inp);
            if (ch == ',') continue;
            if (ch == '}') break;
            exception(ERR_JSON_SYNTAX);
        }
    }
}

static void write_breakpoint_properties(OutputStream * out, BreakpointInfo * bp) {
    BreakpointAttribute * u = bp->unsupported;

    write_stream(out, '{');

    json_write_string(out, "ID");
    write_stream(out, ':');
    json_write_string(out, bp->id);

    if (bp->address != NULL) {
        write_stream(out, ',');
        json_write_string(out, "Location");
        write_stream(out, ':');
        json_write_string(out, bp->address);
    }

    if (bp->condition != NULL) {
        write_stream(out, ',');
        json_write_string(out, "Condition");
        write_stream(out, ':');
        json_write_string(out, bp->condition);
    }

#if SERVICE_LineNumbers
    if (bp->file != NULL) {
        write_stream(out, ',');
        json_write_string(out, "File");
        write_stream(out, ':');
        json_write_string(out, bp->file);
    }

    if (bp->line > 0) {
        write_stream(out, ',');
        json_write_string(out, "Line");
        write_stream(out, ':');
        json_write_long(out, bp->line);
    }

    if (bp->column > 0) {
        write_stream(out, ',');
        json_write_string(out, "Column");
        write_stream(out, ':');
        json_write_long(out, bp->column);
    }
#endif

    if (bp->ignore_count > 0) {
        write_stream(out, ',');
        json_write_string(out, "IgnoreCount");
        write_stream(out, ':');
        json_write_long(out, bp->ignore_count);
    }

    if (bp->enabled) {
        write_stream(out, ',');
        json_write_string(out, "Enabled");
        write_stream(out, ':');
        json_write_boolean(out, bp->enabled);
    }

    while (u != NULL) {
        write_stream(out, ',');
        json_write_string(out, u->name);
        write_stream(out, ':');
        write_string(out, u->value);
        u = u->next;
    }

    write_stream(out, '}');
}

static void send_event_context_added(OutputStream * out, BreakpointInfo * bp) {
    write_stringz(out, "E");
    write_stringz(out, BREAKPOINTS);
    write_stringz(out, "contextAdded");

    write_stream(out, '[');
    write_breakpoint_properties(out, bp);
    write_stream(out, ']');
    write_stream(out, 0);
    write_stream(out, MARKER_EOM);
}

static void send_event_context_changed(OutputStream * out, BreakpointInfo * bp) {
    write_stringz(out, "E");
    write_stringz(out, BREAKPOINTS);
    write_stringz(out, "contextChanged");

    write_stream(out, '[');
    write_breakpoint_properties(out, bp);
    write_stream(out, ']');
    write_stream(out, 0);
    write_stream(out, MARKER_EOM);
}

static void send_event_context_removed(OutputStream * out, BreakpointInfo * bp) {
    write_stringz(out, "E");
    write_stringz(out, BREAKPOINTS);
    write_stringz(out, "contextRemoved");

    write_stream(out, '[');
    json_write_string(out, bp->id);
    write_stream(out, ']');
    write_stream(out, 0);
    write_stream(out, MARKER_EOM);
}

static void add_breakpoint(Channel * c, BreakpointInfo * bp) {
    InputStream * inp = &c->inp;
    BreakpointRef * r = NULL;
    BreakpointInfo * p = NULL;
    int added = 0;
    int chng = 0;
    p = find_breakpoint(bp->id);
    if (p == NULL) {
        int hash = id2bp_hash(bp->id);
        p = (BreakpointInfo *)loc_alloc_zero(sizeof(BreakpointInfo));
        list_init(&p->refs);
        list_add_last(&p->link_all, &breakpoints);
        list_add_last(&p->link_id, id2bp + hash);
        added = 1;
    }
    chng = copy_breakpoint_info(p, bp);
    if (p->deleted) {
        p->deleted = 0;
        added = 1;
    }
    r = find_breakpoint_ref(p, inp);
    if (r == NULL) {
        unsigned inp_hash = (unsigned)inp / 16 % INP2BR_HASH_SIZE;
        r = (BreakpointRef *)loc_alloc_zero(sizeof(BreakpointRef));
        list_add_last(&r->link_inp, inp2br + inp_hash);
        list_add_last(&r->link_bp, &p->refs);
        r->inp = inp;
        r->bp = p;
    }
    else {
        assert(r->bp == p);
        assert(!list_is_empty(&p->refs));
    }
    if (chng || added) {
        if (p->planted || p->enabled && p->unsupported == NULL) replant_breakpoints(c->bcg);
    }
    if (added) send_event_context_added(&c->bcg->out, p);
    else if (chng) send_event_context_changed(&c->bcg->out, p);
}

static void remove_breakpoint(Channel * c, BreakpointInfo * bp) {
    assert(list_is_empty(&bp->refs));
    send_event_context_removed(&c->bcg->out, bp);
    if (bp->planted) {
        bp->deleted = 1;
        replant_breakpoints(c->bcg);
    }
    else {
        free_bp(bp);
    }
}

static void remove_ref(Channel * c, BreakpointRef * br) {
    BreakpointInfo * bp = br->bp;
    list_remove(&br->link_inp);
    list_remove(&br->link_bp);
    loc_free(br);
    if (list_is_empty(&bp->refs)) remove_breakpoint(c, bp);
}

static void delete_breakpoint_refs(Channel * c) {
    InputStream * inp = &c->inp;
    unsigned hash = (unsigned)inp / 16 % INP2BR_HASH_SIZE;
    LINK * l = inp2br[hash].next;
    while (l != &inp2br[hash]) {
        BreakpointRef * br = link_inp2br(l);
        l = l->next;
        if (br->inp == inp) remove_ref(c, br);
    }
}

static void command_ini_bps(char * token, Channel * c) {
    int ch;
    LINK * l = breakpoints.next;
    while (l != &breakpoints) {
        BreakpointInfo * bp = link_all2bp(l);
        l = l->next;
        if (bp->deleted) continue;
        send_event_context_added(&c->out, bp);
        send_event_breakpoint_status(&c->out, bp);
    }

    delete_breakpoint_refs(c);
    
    ch = read_stream(&c->inp);
    if (ch == 'n') {
        if (read_stream(&c->inp) != 'u') exception(ERR_JSON_SYNTAX);
        if (read_stream(&c->inp) != 'l') exception(ERR_JSON_SYNTAX);
        if (read_stream(&c->inp) != 'l') exception(ERR_JSON_SYNTAX);
    }
    else {
        if (ch != '[') exception(ERR_PROTOCOL);
        if (peek_stream(&c->inp) == ']') {
            read_stream(&c->inp);
        }
        else {
            while (1) {
                int ch;
                BreakpointInfo bp;
                read_breakpoint_properties(&c->inp, &bp);
                add_breakpoint(c, &bp);
                ch = read_stream(&c->inp);
                if (ch == ',') continue;
                if (ch == ']') break;
                exception(ERR_JSON_SYNTAX);
            }
        }
    }
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
}

static void command_get_bp_ids(char * token, Channel * c) {
    unsigned hash = (unsigned)&c->inp / 16 % INP2BR_HASH_SIZE;
    LINK * l = inp2br[hash].next;
    int cnt = 0;

    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);
    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);
    write_stream(&c->out, '[');

    while (l != &inp2br[hash]) {
        BreakpointRef * br = link_inp2br(l);
        l = l->next;
        if (br->inp == &c->inp) {
            if (cnt > 0) write_stream(&c->out, ',');
            json_write_string(&c->out, br->bp->id);
            cnt++;
        }
    }

    write_stream(&c->out, ']');
    write_stream(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
}

static void command_get_properties(char * token, Channel * c) {
    char id[256];
    BreakpointInfo * bp = NULL;
    int err = 0;

    json_read_string(&c->inp, id, sizeof(id));
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    bp = find_breakpoint(id);
    if (bp == NULL) err = ERR_INV_CONTEXT;

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, err);
    if (err) {
        write_stringz(&c->out, "null");
    }
    else {
        write_breakpoint_properties(&c->out, bp);
        write_stream(&c->out, 0);
    }
    write_stream(&c->out, MARKER_EOM);
}

static void command_get_status(char * token, Channel * c) {
    char id[256];
    BreakpointInfo * bp = NULL;
    int err = 0;

    json_read_string(&c->inp, id, sizeof(id));
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    bp = find_breakpoint(id);
    if (bp == NULL) err = ERR_INV_CONTEXT;

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, err);
    if (err) {
        write_stringz(&c->out, "null");
    }
    else {
        write_breakpoint_status(&c->out, bp);
        write_stream(&c->out, 0);
    }
    write_stream(&c->out, MARKER_EOM);
}

static void command_bp_add(char * token, Channel * c) {
    BreakpointInfo bp;
    read_breakpoint_properties(&c->inp, &bp);
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    add_breakpoint(c, &bp);

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
}

static void command_bp_change(char * token, Channel * c) {
    BreakpointInfo bp;
    read_breakpoint_properties(&c->inp, &bp);
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    add_breakpoint(c, &bp);

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
}

static void command_bp_enable(char * token, Channel * c) {
    int ch = read_stream(&c->inp);
    if (ch == 'n') {
        if (read_stream(&c->inp) != 'u') exception(ERR_JSON_SYNTAX);
        if (read_stream(&c->inp) != 'l') exception(ERR_JSON_SYNTAX);
        if (read_stream(&c->inp) != 'l') exception(ERR_JSON_SYNTAX);
    }
    else {
        if (ch != '[') exception(ERR_PROTOCOL);
        if (peek_stream(&c->inp) == ']') {
            read_stream(&c->inp);
        }
        else {
            while (1) {
                int ch;
                char id[256];
                BreakpointInfo * bp;
                json_read_string(&c->inp, id, sizeof(id));
                bp = find_breakpoint(id);
                if (bp != NULL && !bp->enabled) {
                    bp->enabled = 1;
                    if (!bp->deleted && bp->unsupported == NULL) replant_breakpoints(c->bcg);
                    send_event_context_changed(&c->bcg->out, bp);
                }
                ch = read_stream(&c->inp);
                if (ch == ',') continue;
                if (ch == ']') break;
                exception(ERR_JSON_SYNTAX);
            }
        }
    }
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
}

static void command_bp_disable(char * token, Channel * c) {
    int ch = read_stream(&c->inp);
    if (ch == 'n') {
        if (read_stream(&c->inp) != 'u') exception(ERR_JSON_SYNTAX);
        if (read_stream(&c->inp) != 'l') exception(ERR_JSON_SYNTAX);
        if (read_stream(&c->inp) != 'l') exception(ERR_JSON_SYNTAX);
    }
    else {
        if (ch != '[') exception(ERR_PROTOCOL);
        if (peek_stream(&c->inp) == ']') {
            read_stream(&c->inp);
        }
        else {
            while (1) {
                int ch;
                char id[256];
                BreakpointInfo * bp;
                json_read_string(&c->inp, id, sizeof(id));
                bp = find_breakpoint(id);
                if (bp != NULL && bp->enabled) {
                    bp->enabled = 0;
                    if (bp->planted) replant_breakpoints(c->bcg);
                    send_event_context_changed(&c->bcg->out, bp);
                }
                ch = read_stream(&c->inp);
                if (ch == ',') continue;
                if (ch == ']') break;
                exception(ERR_JSON_SYNTAX);
            }
        }
    }
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
}

static void command_bp_remove(char * token, Channel * c) {
    int ch = read_stream(&c->inp);
    if (ch == 'n') {
        if (read_stream(&c->inp) != 'u') exception(ERR_JSON_SYNTAX);
        if (read_stream(&c->inp) != 'l') exception(ERR_JSON_SYNTAX);
        if (read_stream(&c->inp) != 'l') exception(ERR_JSON_SYNTAX);
    }
    else {
        if (ch != '[') exception(ERR_PROTOCOL);
        if (peek_stream(&c->inp) == ']') {
            read_stream(&c->inp);
        }
        else {
            while (1) {
                int ch;
                char id[256];
                BreakpointRef * br;
                json_read_string(&c->inp, id, sizeof(id));
                br = find_breakpoint_ref(find_breakpoint(id), &c->inp);
                if (br != NULL) remove_ref(c, br);
                ch = read_stream(&c->inp);
                if (ch == ',') continue;
                if (ch == ']') break;
                exception(ERR_JSON_SYNTAX);
            }
        }
    }
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);
    

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);
    write_stream(&c->out, MARKER_EOM);
}

static void command_get_capabilities(char * token, Channel * c) {
    char id[256];

    json_read_string(&c->inp, id, sizeof(id));
    if (read_stream(&c->inp) != 0) exception(ERR_JSON_SYNTAX);
    if (read_stream(&c->inp) != MARKER_EOM) exception(ERR_JSON_SYNTAX);

    write_stringz(&c->out, "R");
    write_stringz(&c->out, token);
    write_errno(&c->out, 0);

    write_stream(&c->out, '{');
    json_write_string(&c->out, "ID");
    write_stream(&c->out, ':');
    json_write_string(&c->out, id);
    write_stream(&c->out, ',');
    json_write_string(&c->out, "Location");
    write_stream(&c->out, ':');
    json_write_boolean(&c->out, 1);
#if SERVICE_LineNumbers
    write_stream(&c->out, ',');
    json_write_string(&c->out, "File");
    write_stream(&c->out, ':');
    json_write_boolean(&c->out, 1);
    write_stream(&c->out, ',');
    json_write_string(&c->out, "Line");
    write_stream(&c->out, ':');
    json_write_boolean(&c->out, 1);
    write_stream(&c->out, ',');
    json_write_string(&c->out, "Column");
    write_stream(&c->out, ':');
    json_write_boolean(&c->out, 1);
#endif
    write_stream(&c->out, ',');
    json_write_string(&c->out, "IgnoreCount");
    write_stream(&c->out, ':');
    json_write_boolean(&c->out, 1);
    write_stream(&c->out, ',');
    json_write_string(&c->out, "Condition");
    write_stream(&c->out, ':');
    json_write_boolean(&c->out, 1);
    write_stream(&c->out, '}');
    write_stream(&c->out, 0);

    write_stream(&c->out, MARKER_EOM);
}

int is_breakpoint_address(Context * ctx, ContextAddress address) {
    BreakInstruction * bi = find_instruction(ctx, address);
    return bi != NULL && !bi->skip && !bi->error;
}

int evaluate_breakpoint_condition(Context * ctx) {
    int i;
    BreakInstruction * bi = find_instruction(ctx, get_regs_PC(ctx->regs));
    if (bi == NULL) return 0;
    assert(ctx->stopped);
    for (i = 0; i < bi->ref_cnt; i++) {
        BreakpointInfo * bp = bi->refs[i];
        assert(bp->planted);
        assert(bp->error == 0);
        if (bp->deleted) continue;
        if (bp->unsupported != NULL) continue;
        if (!bp->enabled) continue;
        if (bp->condition != NULL) {
            Value v;
            if (evaluate_expression(ctx, STACK_TOP_FRAME, bp->condition, &v) < 0) {
                trace(LOG_ALWAYS, "%s: %s", get_expression_error_msg(), bp->condition);
                return 1;
            }
            if (!value_to_boolean(&v)) continue;
        }
        if (bp->ignore_count > 0) {
            bp->hit_count++;
            if (bp->hit_count < bp->ignore_count) continue;
            bp->hit_count = 0;
        }
        return 1;
    }
    return 0;
}

#ifndef _WRS_KERNEL

static void safe_restore_breakpoint(void * arg) {
    SkipBreakpointInfo * sb = (SkipBreakpointInfo *)arg;
    BreakInstruction * bi = find_instruction(sb->ctx, sb->address);

    if (bi != NULL && bi->skip) {
        assert(bi->error == 0);
        bi->skip = 0;
        plant_instruction(bi);
    }
    if (sb->done) sb->done(sb);
    if (sb->c) stream_unlock(sb->c);
    context_unlock(sb->ctx);
    loc_free(sb);
}

static void safe_skip_breakpoint(void * arg) {
    SkipBreakpointInfo * sb = (SkipBreakpointInfo *)arg;

    assert(!sb->ctx->exited);
    assert(sb->ctx->stopped);
    assert(!sb->ctx->intercepted);
    assert(!sb->ctx->regs_error);
    assert(sb->address == get_regs_PC(sb->ctx->regs));
    
    if (sb->error == 0) {
        BreakInstruction * bi = find_instruction(sb->ctx, sb->address);
        if (bi != NULL && !bi->skip) {
            if (bi->planted) remove_instruction(bi);
            if (bi->error) {
                sb->error = bi->error;
            }
            else {
                bi->skip = 1;
            }
        }
    }
    if (sb->error == 0) {
        post_safe_event(safe_restore_breakpoint, sb);
        if (context_single_step(sb->ctx) < 0) {
            sb->error = errno;
        }
        else if (sb->pending_intercept) {
            sb->ctx->pending_intercept = 1;
        }
    }
    else {
        if (sb->done) sb->done(sb);
        if (sb->c) stream_unlock(sb->c);
        context_unlock(sb->ctx);
        loc_free(sb);
    }
}

#endif

/*
 * When a context is stopped by breakpoint, it is necessary to disable
 * the breakpoint temporarily before the context can be resumed.
 * This function function removes break instruction, then does single step
 * over breakpoint location, then restores break intruction.
 * Return: NULL if it is OK to resume context from current state,
 * SkipBreakpointInfo pointer if context needs to step over a breakpoint.
 */
SkipBreakpointInfo * skip_breakpoint(Context * ctx) {
    BreakInstruction * bi;
    SkipBreakpointInfo * sb;

    assert(!ctx->exited);
    assert(ctx->stopped);

#ifdef _WRS_KERNEL
    /* VxWork debug library can skip breakpoint when neccesary, no code is needed here */
    return NULL;
#else
    if (ctx->exited || ctx->exiting) return NULL;
    assert(!ctx->regs_error);
    bi = find_instruction(ctx, get_regs_PC(ctx->regs));
    if (bi == NULL || bi->error) return NULL;
    assert(!bi->skip);
    sb = (SkipBreakpointInfo *)loc_alloc_zero(sizeof(SkipBreakpointInfo));
    context_lock(ctx);
    sb->ctx = ctx;
    sb->address = get_regs_PC(ctx->regs);
    post_safe_event(safe_skip_breakpoint, sb);
    return sb;
#endif
}

static void event_context_created_or_exited(Context * ctx, void * client_data) {
    if (ctx->parent == NULL) replant_breakpoints((TCFBroadcastGroup *)client_data);
}

static void channel_close_listener(Channel * c) {
    delete_breakpoint_refs(c);
}

void ini_breakpoints_service(Protocol * proto, TCFBroadcastGroup * bcg) {
    int i;
    static ContextEventListener listener = {
        event_context_created_or_exited,
        event_context_created_or_exited
    };
    add_context_event_listener(&listener, bcg);
    list_init(&breakpoints);
    list_init(&instructions);
    for (i = 0; i < ADDR2INSTR_HASH_SIZE; i++) list_init(addr2instr + i);
    for (i = 0; i < ID2BP_HASH_SIZE; i++) list_init(id2bp + i);
    for (i = 0; i < INP2BR_HASH_SIZE; i++) list_init(inp2br + i);
    add_channel_close_listener(channel_close_listener);
    add_command_handler(proto, BREAKPOINTS, "set", command_ini_bps);
    add_command_handler(proto, BREAKPOINTS, "add", command_bp_add);
    add_command_handler(proto, BREAKPOINTS, "change", command_bp_change);
    add_command_handler(proto, BREAKPOINTS, "enable", command_bp_enable);
    add_command_handler(proto, BREAKPOINTS, "disable", command_bp_disable);
    add_command_handler(proto, BREAKPOINTS, "remove", command_bp_remove);
    add_command_handler(proto, BREAKPOINTS, "getBreakpointIDs", command_get_bp_ids);
    add_command_handler(proto, BREAKPOINTS, "getProperties", command_get_properties);
    add_command_handler(proto, BREAKPOINTS, "getStatus", command_get_status);
    add_command_handler(proto, BREAKPOINTS, "getCapabilities", command_get_capabilities);
}

#endif
