/*
 * Copyright 2018, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */
#include <autoconf.h>
#include <utils/util.h>
#include <cspace/cspace.h>
#include <sel4debug/sel4_zf_logif.h>
#include <sel4/sel4.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static inline void *alloc_4k_untyped(cspace_t *cspace, seL4_CPtr *dest)
{
    assert(cspace);
    assert(cspace->alloc.alloc_4k_ut);
    return cspace->alloc.alloc_4k_ut(cspace->alloc.cookie, dest);
}

static inline void free_4k_untyped(cspace_t *cspace, void *untyped)
{
    assert(cspace);
    assert(cspace->alloc.free_4k_ut);
    cspace->alloc.free_4k_ut(cspace->alloc.cookie, untyped);
}

static inline void *map_frame(cspace_t *cspace, seL4_CPtr frame, seL4_CPtr free_slots[MAPPING_SLOTS],
                              seL4_Word *used)
{
    assert(cspace);
    assert(cspace->alloc.map_frame);
    return cspace->alloc.map_frame(cspace->alloc.cookie, frame, free_slots, used);
}

static void *retype_helper(cspace_t *cspace, seL4_Word type, seL4_CPtr *cptr)
{
    seL4_CPtr ut_cptr;
    void *untyped = alloc_4k_untyped(cspace, &ut_cptr);
    if (untyped == NULL) {
        ZF_LOGE("untyped is NULL");
        return NULL;
    }

    /* allocate a slot to retype with */
    *cptr = cspace_alloc_slot(cspace);
    if (*cptr == seL4_CapNull) {
        ZF_LOGE("Failed to allocate slot");
        free_4k_untyped(cspace, untyped);
        return NULL;
    }

    /* we always create 4K objects from this helper however,
     * cnode objects have their size specified in slots, not memory,
     * so adjust the size parameter accordingly */
    size_t size = seL4_PageBits;
    if (type == seL4_CapTableObject) {
        size = CNODE_SLOT_BITS(size);
    }

    /* retype the untyped into the requested type */
    seL4_Error err = cspace_untyped_retype(cspace, ut_cptr, *cptr, type, size);
    if (err != seL4_NoError) {
        ZF_LOGE("error retyping");
        cspace_free_slot(cspace, *cptr);
        free_4k_untyped(cspace, untyped);
        return NULL;
    }

    return untyped;
}

static void refill_watermark(cspace_t *cspace, seL4_Word *used)
{
    for (int i = 0; i < MAPPING_SLOTS; i++) {
        if (*used & BIT(i)) {
            cspace->watermark[i] = cspace_alloc_slot(cspace);
            ZF_LOGW_IF(cspace->watermark[i] != seL4_CapNull, "Cspace full in watermark function");
            break;
        }
    }
}

static bool ensure_levels(cspace_t *cspace, seL4_CPtr cptr, int n_slots, seL4_Word *used)
{
    /* ensure the levels are present for the specific cptr */
    if (!cspace->two_level) {
        /* 1 level cspaces are completely pre-allocated */
        return true;
    }

    seL4_Word node = NODE_INDEX(cptr);

    if (cspace->n_bot_lvl_nodes <= node) {
        /* need to allocate a book keeping page */
        seL4_CPtr frame;
        void *untyped = retype_helper(cspace->bootstrap, seL4_ARM_SmallPageObject, &frame);
        if (untyped == NULL) {
            return false;
        }
        /* map the book keeping frame */
        cspace->bot_lvl_nodes[node] = map_frame(cspace, frame, cspace->watermark, used);
        if (cspace->bot_lvl_nodes[node] == NULL) {
            ZF_LOGD("bot lvl node allocation failed");
            cspace_delete(cspace->bootstrap, frame);
            cspace_free_slot(cspace->bootstrap, frame);
            free_4k_untyped(cspace, untyped);
            return false;
        }
        memset(cspace->bot_lvl_nodes[node], 0, PAGE_SIZE_4K);

        /* now track all the data in the frame */
        cspace->n_bot_lvl_nodes++;
        cspace->bot_lvl_nodes[node]->n_cnodes = 0;
        cspace->bot_lvl_nodes[node]->untyped = untyped;
        cspace->bot_lvl_nodes[node]->frame = frame;
    }

    assert(cspace->bot_lvl_nodes[node] != NULL);
    seL4_Word cnode = CNODE_INDEX(cptr);
    if (cspace->bot_lvl_nodes[node]->n_cnodes < cnode) {
        /* need to allocate a new cnode */
        seL4_CPtr ut_cptr;
        cspace->bot_lvl_nodes[node]->cnodes[cnode].untyped = alloc_4k_untyped(cspace, &ut_cptr);
        if (cspace->bot_lvl_nodes[node]->cnodes[cnode].untyped == NULL) {
            return false;
        }
        /* retype directly into the top level cnode */
        seL4_Error err = seL4_Untyped_Retype(ut_cptr, seL4_CapTableObject, CNODE_SLOT_BITS(CNODE_SIZE_BITS),
                                             cspace->root_cnode, 0, 0, TOP_LVL_INDEX(cptr), 1);
        if (err) {
            ZF_LOGE_IFERR(err, "Failed to retype 2nd lvl cnode");
            free_4k_untyped(cspace, cspace->bot_lvl_nodes[node]->cnodes[cnode].untyped);
            return false;
        }
        cspace->bot_lvl_nodes[node]->n_cnodes++;
    }

    return true;
}


static int cspace_create(cspace_t *cspace, cspace_t *target, bool two_level, cspace_alloc_t cspace_alloc)
{
    memset(target, 0, sizeof(cspace_t));
    target->two_level = two_level;
    /* save the bootstrap cspace for future slot allocation */
    target->bootstrap = cspace;
    target->alloc = cspace_alloc;
    target->top_lvl_size_bits = CNODE_SIZE_BITS;
    /* the top level bf is small, so malloc this memory */
    target->top_bf = calloc(1, BITFIELD_SIZE(target->top_lvl_size_bits));
    if (target->top_bf == NULL) {
        ZF_LOGE("Malloc out of memory");
        return CSPACE_ERROR;
    }

    /* allocate bottom levels (if required) */
    if (target->two_level) {
        target->bot_lvl_nodes = calloc(BOT_LVL_NODES(CNODE_SIZE_BITS), sizeof(bot_lvl_node_t));
        if (target->bot_lvl_nodes == NULL) {
            cspace_destroy(target);
            ZF_LOGE("Malloc out of memory");
            return CSPACE_ERROR;
        }
    }

    /* allocate a slot to mutate the tmp cnode cap into */
    ZF_LOGD("Create top level cspace");
    seL4_CPtr tmp;
    /* retype the untyped into tmp */
    target->untyped = retype_helper(cspace, seL4_CapTableObject, &tmp);
    if (target->untyped == NULL) {
        ZF_LOGE("Failed to retype");
        cspace_destroy(target);
        return CSPACE_ERROR;
    }

    /* Mint the cnode cap with that guard and make it the cap to the root_cnode this cspace --
     * this means that objects in this cspace can be directly invoked with depth seL4_WordBits */
    seL4_Word depth = seL4_WordBits - (CNODE_SLOT_BITS(CNODE_SIZE_BITS) * (target->two_level ? 2 : 1));
    seL4_Word guard = seL4_CNode_CapData_new(0, depth).words[0];
    target->root_cnode = cspace_alloc_slot(cspace);
    if (cspace->root_cnode == seL4_CapNull) {
        ZF_LOGE("Failed to alloc slot");
        cspace_free_slot(target->bootstrap, tmp);
        cspace_destroy(target);
        return CSPACE_ERROR;
    }

    seL4_Error err = cspace_mint(cspace, target->root_cnode, cspace, tmp, seL4_AllRights, guard);
    if (err) {
        ZF_LOGE("Failed to mint root cnode cptr");
        cspace_destroy(target);
        return CSPACE_ERROR;
    }

    cspace_delete(cspace, tmp);
    cspace_free_slot(cspace, tmp);

    /* ensure the cspace levels are there for our first free slot */
    if (target->two_level) {
        if (ensure_levels(target, seL4_CapNull, 0, NULL)) {
            /* now allocate the first slot, to avoid handing out seL4_CapNull */
            seL4_CPtr null = cspace_alloc_slot(target);
            assert(null == seL4_CapNull);

            /* finally, allocate our watermark slots */
            seL4_Word mask = MASK(MAPPING_SLOTS);
            refill_watermark(target, &mask);
        } else {
            ZF_LOGD("Failed to ensure levels on creation");
            cspace_destroy(target);
            return CSPACE_ERROR;
        }
    } else {
        /* allocate slot 0 so its not free */
        seL4_CPtr slot = cspace_alloc_slot(target);
        assert(slot == seL4_CapNull);
    }

    ZF_LOGD("Finished creating new cspace");
    return CSPACE_NOERROR;
}

int cspace_create_two_level(cspace_t *bootstrap, cspace_t *target, cspace_alloc_t alloc)
{
    return cspace_create(bootstrap, target, true, alloc);
}

int cspace_create_one_level(cspace_t *bootstrap, cspace_t *target)
{
    cspace_alloc_t empty = {};
    return cspace_create(bootstrap, target, false, empty);
}

void cspace_destroy(cspace_t *cspace)
{
    if (cspace->bootstrap == NULL) {
        ZF_LOGF("Cannot teardown boostrap cspace");
        return;
    }

    /* free all the bottom level nodes and book keeping */
    for (int i = 0; i < cspace->n_bot_lvl_nodes; i++) {
        for (int j = 0; j < cspace->bot_lvl_nodes[i]->n_cnodes; j++) {
            free_4k_untyped(cspace, cspace->bot_lvl_nodes[i]->cnodes[j].untyped);
        }
        cspace_delete(cspace->bootstrap, cspace->bot_lvl_nodes[i]->frame);
        cspace_free_slot(cspace->bootstrap, cspace->bot_lvl_nodes[i]->frame);
        free_4k_untyped(cspace, cspace->bot_lvl_nodes[i]->untyped);
    }

    /* free the top level cnode */
    if (cspace->root_cnode != seL4_CapNull) {
        cspace_delete(cspace->bootstrap, cspace->root_cnode);
        cspace_free_slot(cspace->bootstrap, cspace->root_cnode);
    }

    /* give the untyped back to the untyped manager */
    if (cspace->untyped)  {
        free_4k_untyped(cspace, cspace->untyped);
    }

    if (cspace->bot_lvl_nodes) {
        free(cspace->bot_lvl_nodes);
    }

    if (cspace->top_bf) {
        free(cspace->top_bf);
    }
}

seL4_CPtr cspace_alloc_slot(cspace_t *cspace)
{
    assert(cspace != NULL);
    seL4_Word top_index = bf_first_free(BITFIELD_SIZE(cspace->top_lvl_size_bits), cspace->top_bf);
    if (top_index == CNODE_SLOTS(cspace->top_lvl_size_bits)) {
        ZF_LOGE("Cspace is full!\n");
        return seL4_CapNull;
    }

    seL4_CPtr cptr;
    if (cspace->two_level) {
        seL4_Word used = 0;
        /* which cnode to use */
        cptr = top_index << CNODE_SLOT_BITS(CNODE_SIZE_BITS);

        /* ensure the bottom level cnode is present */
        if (cspace->n_bot_lvl_nodes < NODE_INDEX(cptr) ||
                cspace->bot_lvl_nodes[NODE_INDEX(cptr)]->n_cnodes < CNODE_INDEX(cptr)) {
            if (!ensure_levels(cspace, cptr, MAPPING_SLOTS, &used)) {
                return seL4_CapNull;
            }
        }

        /* now allocate a bottom level index */
        bot_lvl_t *bot_lvl = &cspace->bot_lvl_nodes[NODE_INDEX(cptr)]->cnodes[CNODE_INDEX(cptr)];
        seL4_Word bot_index = bf_first_free(BITFIELD_SIZE(CNODE_SIZE_BITS), bot_lvl->bf);
        if (bot_index == CNODE_SLOTS(CNODE_SIZE_BITS)) {
            ZF_LOGE("Cspace is full!\n");
            return seL4_CapNull;
        }

        bf_set_bit(bot_lvl->bf, bot_index);
        if (bot_index == seL4_WordBits - 1) {
            /* we just allocated the last slot -> mark the top level as full */
            bf_set_bit(cspace->top_bf, top_index);
        }

        cptr += bot_index;

        /* now refill the watermark if required */
        refill_watermark(cspace, &used);
    } else {
        cptr = top_index;
        bf_set_bit(cspace->top_bf, cptr);
    }
    return cptr;
}

void cspace_free_slot(cspace_t *cspace, seL4_CPtr cptr)
{
    if (!cspace->two_level) {
        if (cptr > CNODE_SLOTS(cspace->top_lvl_size_bits)) {
            ZF_LOGE("Attempting to delete slot greater than cspace bounds");
            return;
        }
        bf_clr_bit(cspace->top_bf, cptr);
    } else {
        if (cptr > CNODE_SLOTS(CNODE_SIZE_BITS + cspace->top_lvl_size_bits)) {
            ZF_LOGE("Attempting to delete slot greater than cspace bounds");
        }

        bf_clr_bit(cspace->top_bf, TOP_LVL_INDEX(cptr));
        seL4_Word node = NODE_INDEX(cptr);
        if (cspace->n_bot_lvl_nodes > node) {
            seL4_Word cnode = CNODE_INDEX(cptr);
            if (cspace->bot_lvl_nodes[node]->n_cnodes > cnode) {
                bf_clr_bit(cspace->bot_lvl_nodes[node]->cnodes[cnode].bf, BOT_LVL_INDEX(cptr));
            } else {
                ZF_LOGE("Attempting to free unallocated cptr!");
            }
        } else {
            ZF_LOGE("Attempting to free unallocated cptr!");
        }
    }
}

seL4_Error cspace_untyped_retype(cspace_t *cspace, seL4_CPtr ut, seL4_CPtr target,
                                 seL4_Word type, size_t size_bits)
{

    if (cspace->two_level) {
        /* we need to retype directly into the 2nd level cnode */
        seL4_CPtr cnode = target >> CNODE_SLOT_BITS(CNODE_SIZE_BITS);
        return seL4_Untyped_Retype(ut, type, size_bits, cspace->root_cnode, cnode,
                                   seL4_WordBits - CNODE_SLOT_BITS(CNODE_SIZE_BITS),
                                   target % CNODE_SLOTS(CNODE_SIZE_BITS), 1);
    } else {
        /* if its a 1 level, we can retype directly into the 1 level cnode */
        return seL4_Untyped_Retype(ut, type, size_bits, cspace->root_cnode, 0, 0, target, 1);

    }
}