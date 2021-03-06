/*
 * caam - Freescale Integrated Security Engine (SEC) device driver
 * Support for off-loading XOR Parity Calculations to CAAM.
 *
 * Copyright 2011 Freescale Semiconductor, Inc
 *
 * relationship between job descriptors, shared descriptors and sources:
 * ------------------------------           -------------------
 * | ShareDesc                  |<------\   | JobDesc         |
 * |   Load src pointers to ctx |        \--| ShareDesc ptr   |
 * | new src jump dst:          |<-----\    | SEQ_OUT_PTR     |
 * |   Load ith src             |      |    | (output buffer) |
 * | new src mv dst:            |      |    | (output length) |
 * |   (ith src commands)       |      |    | SEQ_IN_PTR      |
 * | load:                      |<---\ |    | (src commands)  |----\
 * |   Seq load chunk           |    | |    -------------------    |
 * | return:                    |<---|-|-\                         |
 * |   XOR quarter chunk        |    | | |                         |
 * |   Pass complete?           |----^-^---\                       |
 * |   Half chunk left?         |----^-+ | |                       |
 * |   Default                  |----^-^-+ |                       |
 * | store:                     |<---|-|-|-/                       |
 * |   Seq store chunk          |    | | |    -------------------  |
 * |   No data left to write?   |X   | | |    | first src ptr   |<-/
 * |   Put src1 chunk in result |    | | |    | first src len   |
 * |   Default                  |----^-+ |  /-| shared hdr jump |
 * | first:                     |<---|-|-|-/  | nop (if needed) |
 * |   No data left to read?    |----^-^-+    -------------------
 * |   Seq load chunk           |    | | |    | ith src ptr     |
 * |   Load src2                |    | | |    | ith src len     |
 * |   Not first pass?          |----^-^-/    | load src i + 1  |
 * | first pass:                |    | |      | nop (if needed) |
 * |   Put src1 chunk in result |    | |      -------------------
 * |   set output size          |    | |      | last src ptr    |
 * |   Default                  |----^-/      | last src len    |
 * | last:                      |<---|--------| shared hdr jump |
 * |   Update index             |    |        | nop (if needed) |
 * |   Load src1                |    |        -------------------
 * |   Default                  |----/
 * ------------------------------
 *
 */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/dmaengine.h>

#include "compat.h"
#include "regs.h"
#include "jr.h"
#include "error.h"
#include "intern.h"
#include "desc.h"
#include "desc_constr.h"

#define MAX_INITIAL_DESCS	64
#define MAX_XOR_SRCS		8

#define JOB_DESC_BYTES		(4 * CAAM_CMD_SZ + 3 * CAAM_PTR_SZ)
#define JOB_DESC_LEN		(JOB_DESC_BYTES / CAAM_CMD_SZ)
#define CMD_DESC_LEN		32

#define LONG_PTR		(CAAM_PTR_SZ > CAAM_CMD_SZ)

#define CTX1_SLOTS		4
#define SRC_CMD_BYTES		(4 * CAAM_CMD_SZ)
#define SRC_CMD_LEN		(SRC_CMD_BYTES / CAAM_CMD_SZ)
#define CHUNK_SIZE		128
#define CHUNK_SIZE_H		64
#define CHUNK_SIZE_Q		32
#define REG_SIZE		8

#define CMD_MOVE_OVERFLOW_LEN	1

#define LABEL_SRC_JMP_BYTES	(5 * CAAM_CMD_SZ)
#define LABEL_SRC_JMP		(LABEL_SRC_JMP_BYTES / CAAM_CMD_SZ)
#define LABEL_SRC_MV_BYTES	(CAAM_CMD_SZ + LABEL_SRC_JMP_BYTES)
#define LABEL_SRC_MV		(LABEL_SRC_MV_BYTES / CAAM_CMD_SZ)
#define LABEL_FIRST_BYTES	(28 * CAAM_CMD_SZ + LABEL_SRC_MV_BYTES)
#define LABEL_FIRST		(LABEL_FIRST_BYTES / CAAM_CMD_SZ)
#define LABEL_LAST_BYTES	(13 * CAAM_CMD_SZ + LABEL_FIRST_BYTES)
#define LABEL_LAST		(LABEL_LAST_BYTES / CAAM_CMD_SZ)
#define SH_DESC_BYTES		(5 * CAAM_CMD_SZ + LABEL_LAST_BYTES)
#define SH_DESC_LEN		(SH_DESC_BYTES / CAAM_CMD_SZ)

#ifdef DEBUG
/* for print_hex_dumps with line references */
#define xstr(s) str(s)
#define str(s) #s
#define debug(format, arg...) printk(format, arg)
#else
#define debug(format, arg...)
#endif

struct caam_xor_sh_desc {
	u32 desc[SH_DESC_LEN + CMD_MOVE_OVERFLOW_LEN];
	dma_addr_t sh_desc_phys;
};

struct caam_dma_async_tx_desc {
	struct dma_async_tx_descriptor async_tx;
	struct list_head node;
	struct caam_dma_jr *dma_jr;
	u32 job_desc[JOB_DESC_LEN];
	u32 cmd_desc[CMD_DESC_LEN];
	dma_addr_t cmd_desc_phys;
	dma_addr_t dest;
	dma_addr_t src[MAX_XOR_SRCS];
	u32 src_cnt;
	u32 dma_len;
};

struct caam_dma_desc_pool {
	int desc_cnt;
	struct list_head head;
};

/*
 * caam_dma_jr - job ring/channel data
 * @completed_cookie: cookie of latest latest, completed job
 * @timer_list: timer to issue pending tx
 * @chan: dma channel used by async_tx API
 * @desc_lock: lock on job descriptor
 * @submit_q: queue of pending (submitted, but not enqueued) jobs
 * @done_lock: lock on done_not_acked
 * @done_not_acked: jobs that have been completed by jr, but maybe not acked
 * @caam_hw_jr: jr device data
 * @pool_lock: lock on soft_desc
 * @soft_desc: pool of pre-allocated caam_dma_async_tx_desc structures
 */
struct caam_dma_jr {
	struct dma_chan chan;
	struct device *dev;
	struct timer_list timer;
	spinlock_t desc_lock;
	struct caam_drv_private_jr *caam_hw_jr;
	dma_cookie_t completed_cookie;
	struct list_head submit_q;
	spinlock_t done_lock;
	struct list_head done_not_acked;
	spinlock_t pool_lock;
	struct caam_dma_desc_pool *soft_desc;
};

static inline u32 load_source(u32 ctx, u32 offset, u32 target)
{
	return ctx | MOVE_DEST_DESCBUF | SRC_CMD_BYTES |
	       (target << (2 + MOVE_OFFSET_SHIFT)) |
	       (offset << MOVE_AUX_SHIFT);
}

static inline u32 *write_load_source(u32 *desc, u32 ctx, u32 offset, u32 target)
{
	return write_move(desc, load_source(ctx, offset, target));
}

/* generate source commands and job descriptor for each request */
static void prepare_caam_xor_desc(struct device *dev,
				  struct caam_dma_async_tx_desc *desc,
				  dma_addr_t sh_desc_phys,
				  dma_addr_t dest, dma_addr_t *src,
				  u32 src_cnt, size_t len)
{
	u32 label_src_mv = LABEL_SRC_MV + CMD_MOVE_OVERFLOW_LEN;
	u32 label_first = LABEL_FIRST + CMD_MOVE_OVERFLOW_LEN;
	u32 label_last = LABEL_LAST + CMD_MOVE_OVERFLOW_LEN;
	u32 sh_desc_len = SH_DESC_LEN + CMD_MOVE_OVERFLOW_LEN;
	int i;
	u32 *job_descptr = desc->job_desc;
	u32 *cmd_desc = desc->cmd_desc;

	desc->dest = dest;
	memcpy(desc->src, src, src_cnt*sizeof(dma_addr_t));
	desc->src_cnt = src_cnt;
	desc->dma_len = len;

	/* first source: jump to special commands */
	cmd_desc = write_ptr(cmd_desc, src[0]);
	cmd_desc = write_cmd(cmd_desc, len);
	init_sh_desc(cmd_desc, (label_first & HDR_START_IDX_MASK) <<
		      HDR_START_IDX_SHIFT);
	cmd_desc++;
	if (!LONG_PTR)
		cmd_desc = write_nop(cmd_desc, 1);

	i = 1;
	/* sources that load next source from first context */
	while (i < src_cnt - 1 && i < CTX1_SLOTS - 1) {
		cmd_desc = write_ptr(cmd_desc, src[i]);
		cmd_desc = write_cmd(cmd_desc, len);
		cmd_desc = write_load_source(cmd_desc, MOVE_SRC_CLASS1CTX, i +
					     1, label_src_mv);
		if (!LONG_PTR)
			cmd_desc = write_nop(cmd_desc, 1);
		i++;
	}
	/* sources that load next source from second context */
	while (i < src_cnt - 1) {
		cmd_desc = write_ptr(cmd_desc, src[i]);
		cmd_desc = write_cmd(cmd_desc, len);
		cmd_desc = write_load_source(cmd_desc, MOVE_SRC_CLASS2CTX, i +
					     1, label_src_mv);
		if (!LONG_PTR)
			cmd_desc = write_nop(cmd_desc, 1);
		i++;
	}

	/* last source: jump to special commands */
	cmd_desc = write_ptr(cmd_desc, src[i]);
	cmd_desc = write_cmd(cmd_desc, len);
	init_sh_desc(cmd_desc, (label_last & HDR_START_IDX_MASK) <<
		      HDR_START_IDX_SHIFT);
	cmd_desc++;
	if (!LONG_PTR)
		cmd_desc = write_nop(cmd_desc, 1);

	desc->cmd_desc_phys = dma_map_single(dev, desc->cmd_desc,
					     CMD_DESC_LEN * sizeof(u32),
					     DMA_TO_DEVICE);
	init_job_desc_shared(job_descptr, sh_desc_phys, sh_desc_len,
			     HDR_SHARE_WAIT | HDR_REVERSE);

	append_seq_out_ptr(job_descptr, dest, len, 0);
	append_seq_in_ptr_intlen(job_descptr, desc->cmd_desc_phys,
					MAX_XOR_SRCS * SRC_CMD_BYTES, 0);

#ifdef DEBUG
	print_hex_dump(KERN_ERR, "job desc @"xstr(__LINE__)": ",
		       DUMP_PREFIX_ADDRESS, 16, 4, job_descptr, CAAM_CMD_SZ *
		       desc_len(job_descptr), 1);
	print_hex_dump(KERN_ERR, "srcs @"xstr(__LINE__)": ",
		       DUMP_PREFIX_ADDRESS, 16, 4, src, src_cnt * CAAM_PTR_SZ,
		       1);
	print_hex_dump(KERN_ERR, "src cmd@"xstr(__LINE__)": ",
		       DUMP_PREFIX_ADDRESS, 16, 4, desc->cmd_desc,
		       SRC_CMD_BYTES * src_cnt, 1);
#endif
}

/* generate shared descriptor for each device */
static void prepare_caam_xor_sh_desc(u32 *descptr, u32 src_cnt)
{
	bool overflow;
	u32 label_src_jmp, label_src_mv;
	u32 *store_jump_cmd;
	u32 label_load, label_return, label_store;

	overflow = src_cnt > CTX1_SLOTS;
	label_src_jmp = LABEL_SRC_JMP + CMD_MOVE_OVERFLOW_LEN;
	label_src_mv = label_src_jmp + 1;
	init_sh_desc(descptr, HDR_SHARE_SERIAL);
	/* Store up to 4 sources in ctx1 */
	append_cmd(descptr, CMD_SEQ_LOAD | LDST_SRCDST_BYTE_CONTEXT |
		   LDST_CLASS_1_CCB | (overflow ?
		   (CTX1_SLOTS * SRC_CMD_BYTES) : (src_cnt * SRC_CMD_BYTES)));

	/* Store any overflow in ctx2 */
	if (overflow)
		append_cmd(descptr, CMD_SEQ_LOAD | LDST_SRCDST_BYTE_CONTEXT |
			   LDST_CLASS_2_CCB | (src_cnt - 4) * 16);
	else
		append_cmd(descptr, CMD_SEQ_LOAD | LDST_SRCDST_BYTE_CONTEXT |
			   LDST_CLASS_2_CCB | 4 * 16);

	append_cmd(descptr, CMD_LOAD | DISABLE_AUTO_INFO_FIFO);

	/* Load first source */
	append_move(descptr, load_source(MOVE_SRC_CLASS1CTX, 0, label_src_mv) |
		    MOVE_WAITCOMP);

	/* Refresh shared descriptor */
	append_cmd(descptr, CMD_SHARED_DESC_HDR | HDR_SHARE_NEVER | HDR_ONE |
		   ((label_src_jmp & HDR_START_IDX_MASK) <<
		    HDR_START_IDX_SHIFT));

	/* Load source and run loaded commands */
	append_cmd(descptr, CMD_SEQ_IN_PTR | SQIN_EXT);
	append_len(descptr, SRC_CMD_LEN);

	/* Skip read data */
	append_seq_fifo_load(descptr, 0, KEY_VLF | FIFOLD_CLASS_SKIP);

	/* Load chunk to ififo */
	label_load = desc_len(descptr);
	append_seq_fifo_load(descptr, CHUNK_SIZE, FIFOLD_TYPE_PK |
			     LDST_CLASS_1_CCB);

	/* Update added number of bytes in ififo */
	append_math_add_imm_u32(descptr, VARSEQOUTLEN, VARSEQOUTLEN, IMM,
				CHUNK_SIZE);

	/* Load chunk from ififo to math registers via DECO alignment block*/
	append_load_imm_u32(descptr, NFIFOENTRY_LC1 | NFIFOENTRY_DTYPE_MSG |
			    CHUNK_SIZE, LDST_SRCDST_WORD_INFO_FIFO);
	label_return = desc_len(descptr);
	append_move(descptr, MOVE_WAITCOMP | MOVE_SRC_INFIFO |
		    MOVE_DEST_MATH0 | CHUNK_SIZE_Q);

	/* XOR math registers with ofifo */
	append_math_xor(descptr, REG0, REG0, OUTFIFO, REG_SIZE);
	append_math_xor(descptr, REG1, REG1, OUTFIFO, REG_SIZE);
	append_math_xor(descptr, REG2, REG2, OUTFIFO, REG_SIZE);
	append_math_xor(descptr, REG3, REG3, OUTFIFO, REG_SIZE);

	/* Move result to ofifo */
	append_move(descptr, MOVE_SRC_MATH0 | MOVE_WAITCOMP |
		    MOVE_DEST_OUTFIFO | CHUNK_SIZE_Q);

	/* Update reduced number of bytes in ififo */
	append_math_sub_imm_u32(descptr, VARSEQOUTLEN, VARSEQOUTLEN, IMM,
				CHUNK_SIZE_Q);

	/* If ififo has no more data, store chunk */
	store_jump_cmd = append_jump(descptr, JUMP_TEST_ALL |
				     JUMP_COND_MATH_Z);

	/* If half of chunk left, use next source */
	append_math_sub_imm_u32(descptr, NONE, VARSEQOUTLEN, IMM,
				CHUNK_SIZE_H);
	append_jump_to(descptr, JUMP_TEST_ALL | JUMP_COND_MATH_Z,
		       label_src_jmp);

	/* Else, keep XORing */
	append_jump_to(descptr, 0, label_return);

	/* Store */
	label_store = desc_len(descptr);
	set_jump_tgt_here(descptr, store_jump_cmd);

	/* Store chunk to seqout */
	append_seq_fifo_store(descptr, CHUNK_SIZE, FIFOST_TYPE_MESSAGE_DATA);

	/* Halt if no more data */
	append_math_sub(descptr, NONE, SEQOUTLEN, ONE, CAAM_CMD_SZ);
	append_jump(descptr, JUMP_TYPE_HALT_USER | JUMP_TEST_ALL |
			JUMP_COND_MATH_N);

	/* Load first source's next chunk to ofifo */
	append_move(descptr, MOVE_SRC_INFIFO | MOVE_DEST_OUTFIFO |
			MOVE_WAITCOMP | CHUNK_SIZE);

	/* Goto source */
	append_cmd(descptr, CMD_SHARED_DESC_HDR | HDR_SHARE_NEVER | HDR_ONE |
		   ((label_src_jmp & HDR_START_IDX_MASK) <<
		     HDR_START_IDX_SHIFT));

	/* First source, skip read data */
	append_seq_fifo_load(descptr, 0, KEY_VLF | FIFOLD_CLASS_SKIP);

	/* If no more data to read, go XOR read data */
	append_math_sub(descptr, NONE, SEQINLEN, ONE, CAAM_CMD_SZ);
	append_jump_to(descptr, JUMP_TEST_ALL | JUMP_COND_MATH_N,
		       label_return);

	/* Otherwise, load chunk from first source to DECO alignment block */
	append_seq_fifo_load(descptr, CHUNK_SIZE, FIFOLD_TYPE_PK |
			     LDST_CLASS_1_CCB);
	append_load_imm_u32(descptr, NFIFOENTRY_LC1 | NFIFOENTRY_DTYPE_MSG |
			    CHUNK_SIZE, LDST_SRCDST_WORD_INFO_FIFO);

	/* Load second source */
	append_move(descptr, load_source(MOVE_SRC_CLASS1CTX, 1, label_src_mv));

	/* XOR previous pass if this is not first pass */
	append_math_sub(descptr, NONE, VARSEQINLEN, ONE, CAAM_CMD_SZ);
	append_jump_to(descptr, JUMP_TEST_INVALL | JUMP_COND_MATH_N,
		       label_return);

	/* Else, move chunk for DECO alignment block to ofifo */
	append_move(descptr, MOVE_SRC_INFIFO | MOVE_DEST_OUTFIFO |
				MOVE_WAITCOMP | CHUNK_SIZE);

	/* and track number of bytes to write*/
	append_math_add_imm_u32(descptr, SEQOUTLEN, SEQINLEN, IMM, CHUNK_SIZE);

	/* Goto source */
	append_cmd(descptr, CMD_SHARED_DESC_HDR | HDR_SHARE_NEVER | HDR_ONE |
		   ((label_src_jmp & HDR_START_IDX_MASK) <<
		     HDR_START_IDX_SHIFT));

	/* Last source, skip read data */
	append_seq_fifo_load(descptr, 0, KEY_VLF | FIFOLD_CLASS_SKIP);

	/* Update number of bytes to skip */
	append_math_add_imm_u32(descptr, VARSEQINLEN, VARSEQINLEN, IMM,
				CHUNK_SIZE);

	/* Load first source */
	append_move(descptr, load_source(MOVE_SRC_CLASS1CTX, 0, label_src_mv));

	/* Goto data loading */
	append_cmd(descptr, CMD_SHARED_DESC_HDR | HDR_SHARE_NEVER | HDR_ONE |
		   ((label_load & HDR_START_IDX_MASK) << HDR_START_IDX_SHIFT));

#ifdef DEBUG
	print_hex_dump(KERN_ERR, "shdesc @"xstr(__LINE__)": ",
		       DUMP_PREFIX_ADDRESS, 16, 4, descptr, CAAM_CMD_SZ *
		       desc_len(descptr), 1);
#endif
}

static enum dma_status caam_jr_tx_status(struct dma_chan *chan,
					 dma_cookie_t cookie,
					 struct dma_tx_state *txstate)
{
	struct caam_dma_jr *jr = NULL;
	dma_cookie_t last_used;
	dma_cookie_t last_complete;

	jr = container_of(chan, struct caam_dma_jr, chan);

	last_used = chan->cookie;
	last_complete = jr->completed_cookie;

	dma_set_tx_state(txstate, last_complete, last_used, 0);

	return dma_async_is_complete(cookie, last_complete, last_used);
}

static dma_cookie_t caam_jr_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct caam_dma_async_tx_desc *desc = NULL;
	struct caam_dma_jr *jr = NULL;
	dma_cookie_t cookie;

	desc = container_of(tx, struct caam_dma_async_tx_desc, async_tx);
	jr = container_of(tx->chan, struct caam_dma_jr, chan);

	spin_lock_bh(&jr->desc_lock);
	jr->timer.data = (unsigned long)tx->chan;
	cookie = jr->chan.cookie + 1;
	if (cookie < DMA_MIN_COOKIE)
		cookie = DMA_MIN_COOKIE;

	desc->async_tx.cookie = cookie;
	jr->chan.cookie = desc->async_tx.cookie;
	list_add_tail(&desc->node, &jr->submit_q);

	if (!timer_pending(&jr->timer))
		add_timer(&jr->timer);

	spin_unlock_bh(&jr->desc_lock);

	return cookie;
}

static struct caam_dma_async_tx_desc *
caam_jr_chan_alloc_desc(struct dma_chan *chan)
{
	struct caam_dma_jr *jr = container_of(chan, struct caam_dma_jr, chan);
	struct caam_dma_async_tx_desc *desc = NULL;

	spin_lock_bh(&jr->pool_lock);
	if (jr->soft_desc->desc_cnt) {
		desc = container_of(jr->soft_desc->head.next,
				    struct caam_dma_async_tx_desc, node);
		jr->soft_desc->desc_cnt--;
		list_del(&desc->node);
	}
	spin_unlock_bh(&jr->pool_lock);

	if (!desc) {
		desc = kzalloc(sizeof(struct caam_dma_async_tx_desc),
			       GFP_KERNEL | GFP_DMA);
		if (!desc) {
			dev_err(jr->dev, "cannot alloc dma mem for XOR desc\n");
			return ERR_PTR(-ENOMEM);
		}
		desc->async_tx.tx_submit = caam_jr_tx_submit;
	}

	return desc;
}

static void caam_jr_chan_free_desc(struct caam_dma_async_tx_desc *desc)
{
	struct caam_dma_jr *dma_jr = desc->dma_jr;
	struct caam_dma_async_tx_desc *_desc = NULL;

	spin_lock_bh(&dma_jr->done_lock);

	list_add_tail(&desc->node, &dma_jr->done_not_acked);
	list_for_each_entry_safe(desc, _desc, &dma_jr->done_not_acked, node) {
		if (async_tx_test_ack(&desc->async_tx)) {
			list_del(&desc->node);

			spin_lock_bh(&dma_jr->pool_lock);
			if (dma_jr->soft_desc->desc_cnt < MAX_INITIAL_DESCS) {
				INIT_LIST_HEAD(&desc->node);
				list_add(&desc->node, &dma_jr->soft_desc->head);
				dma_jr->soft_desc->desc_cnt++;
			} else {
				kfree(desc);
			}
			spin_unlock_bh(&dma_jr->pool_lock);
		}
	}

	spin_unlock_bh(&dma_jr->done_lock);
}

static void caam_dma_xor_done(struct device *dev, u32 *hwdesc, u32 status,
			      void *auxarg)
{
	struct caam_dma_async_tx_desc *desc;
	struct caam_dma_jr *dma_jr;
	dma_async_tx_callback callback;
	void *callback_param;
	struct device *jrdev;
	enum dma_ctrl_flags flags;

	desc = (struct caam_dma_async_tx_desc *)auxarg;
	dma_jr = desc->dma_jr;
	jrdev = dma_jr->caam_hw_jr->parentdev;
	flags = desc->async_tx.flags;

	if (status) {
		char tmp[256];
		dev_err(dev, "%s\n", caam_jr_strstatus(tmp, status));
	}

	dma_run_dependencies(&desc->async_tx);

	spin_lock_bh(&dma_jr->desc_lock);
	if (dma_jr->completed_cookie < desc->async_tx.cookie) {
		dma_jr->completed_cookie = desc->async_tx.cookie;
		if (dma_jr->completed_cookie == DMA_MAX_COOKIE)
			dma_jr->completed_cookie = DMA_MIN_COOKIE;
	}
	spin_unlock_bh(&dma_jr->desc_lock);

	callback = desc->async_tx.callback;
	callback_param = desc->async_tx.callback_param;

	dma_unmap_single(jrdev, desc->cmd_desc_phys,
			CMD_DESC_LEN * sizeof(u32), DMA_TO_DEVICE);

	if (likely(!(flags & DMA_COMPL_SKIP_DEST_UNMAP)))
		dma_unmap_page(jrdev, desc->dest, desc->dma_len,
						DMA_BIDIRECTIONAL);

	if (likely(!(flags & DMA_COMPL_SKIP_SRC_UNMAP))) {
		u32 i;
		for (i = 0; i < desc->src_cnt; i++) {
			if (desc->src[i] == desc->dest)
				continue;
			dma_unmap_page(jrdev, desc->src[i],
					desc->dma_len, DMA_TO_DEVICE);
		}
	}

	caam_jr_chan_free_desc(desc);

	if (callback)
		callback(callback_param);
}

static void caam_jr_issue_pending(struct dma_chan *chan)
{
	struct caam_dma_jr *dma_jr = NULL;
	struct caam_dma_async_tx_desc *desc, *_desc;
	struct device *dev;

	dma_jr = container_of(chan, struct caam_dma_jr, chan);
	dev = dma_jr->dev;
	if (timer_pending(&dma_jr->timer))
		del_timer_sync(&dma_jr->timer);

	spin_lock_bh(&dma_jr->desc_lock);
	list_for_each_entry_safe(desc, _desc, &dma_jr->submit_q, node) {
		desc->dma_jr = dma_jr;
		if (caam_jr_enqueue(dev, desc->job_desc,
				    caam_dma_xor_done, desc) < 0)
			break;

		list_del(&desc->node);
	}

	spin_unlock_bh(&dma_jr->desc_lock);
}

static struct dma_async_tx_descriptor *
caam_jr_prep_dma_xor(struct dma_chan *chan, dma_addr_t dest, dma_addr_t *src,
		     unsigned int src_cnt, size_t len, unsigned long flags)
{
	struct caam_dma_jr *jr = NULL;
	struct caam_dma_async_tx_desc *desc = NULL;
	struct caam_drv_private *priv;

	jr = container_of(chan, struct caam_dma_jr, chan);

	if (src_cnt > MAX_XOR_SRCS) {
		dev_err(jr->dev, "%d XOR srcs, exceed max number: %d\n",
				src_cnt, MAX_XOR_SRCS);
		return NULL;
	}

	desc = caam_jr_chan_alloc_desc(chan);
	if (desc < 0)
		return ERR_PTR(-ENOMEM);

	dma_async_tx_descriptor_init(&desc->async_tx, &jr->chan);

	priv = dev_get_drvdata(jr->caam_hw_jr->parentdev);

	prepare_caam_xor_desc(jr->caam_hw_jr->parentdev, desc,
			      priv->xor_sh_desc[0].sh_desc_phys, dest,
			      src, src_cnt, len);

	desc->async_tx.flags = flags;
	desc->async_tx.cookie = -EBUSY;
	return &desc->async_tx;
}

static void caam_jr_free_chan_resources(struct dma_chan *chan)
{
	struct caam_dma_jr *jr = container_of(chan, struct caam_dma_jr, chan);
	struct caam_dma_async_tx_desc *desc;
	struct list_head *current_node;

	spin_lock_bh(&jr->pool_lock);
	current_node = jr->soft_desc->head.next;
	while (jr->soft_desc->desc_cnt > 0) {
		desc = container_of(current_node, struct caam_dma_async_tx_desc,
				    node);
		current_node = current_node->next;
		list_del(&desc->node);
		kfree(desc);
		jr->soft_desc->desc_cnt--;
	}

	kfree(jr->soft_desc);
	spin_unlock_bh(&jr->pool_lock);
}

static int caam_jr_alloc_chan_resources(struct dma_chan *chan)
{
	struct caam_dma_jr *jr = container_of(chan, struct caam_dma_jr, chan);
	struct caam_dma_async_tx_desc *desc;
	unsigned int i;

	jr->soft_desc = kzalloc(sizeof(struct caam_dma_desc_pool),
			GFP_KERNEL | GFP_DMA);
	if (!jr->soft_desc) {
		pr_err("%s: cannot alloc resources for DMA chan\n",
		       __func__);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&jr->soft_desc->head);
	for (i = 0; i < MAX_INITIAL_DESCS; i++) {
		desc = kzalloc(sizeof(struct caam_dma_async_tx_desc),
			       GFP_KERNEL | GFP_DMA);
		if (!desc)
			return -ENOMEM;

		desc->async_tx.tx_submit = caam_jr_tx_submit;
		spin_lock_bh(&jr->pool_lock);
		jr->soft_desc->desc_cnt++;
		list_add_tail(&desc->node, &jr->soft_desc->head);
		spin_unlock_bh(&jr->pool_lock);
	}

	return 0;
}

static void caam_jr_timer_handler(unsigned long data)
{
	struct dma_chan *chan = (struct dma_chan *)data;
	caam_jr_issue_pending(chan);
}

static int caam_jr_chan_bind(struct device *ctrldev, struct device *dev)
{
	struct caam_drv_private *priv = dev_get_drvdata(ctrldev);
	struct caam_drv_private_jr *jrpriv = dev_get_drvdata(dev);
	struct dma_device *dma_dev = &priv->dma_dev;
	struct caam_dma_jr *dma_jr;

	dma_jr = kzalloc(sizeof(struct caam_dma_jr), GFP_KERNEL);
	if (!dma_jr) {
		dev_err(dev, "cannot alloc mem for caam job queue\n");
		return -ENOMEM;
	}

	dma_jr->chan.device = dma_dev;
	dma_jr->chan.private = dma_jr;

	INIT_LIST_HEAD(&dma_jr->submit_q);
	spin_lock_init(&dma_jr->desc_lock);
	spin_lock_init(&dma_jr->pool_lock);
	init_timer(&dma_jr->timer);
	dma_jr->timer.expires = jiffies + 10 * HZ;
	dma_jr->timer.function = caam_jr_timer_handler;

	list_add_tail(&dma_jr->chan.device_node, &dma_dev->channels);

	dma_jr->caam_hw_jr = jrpriv;
	dma_jr->dev = dev;
	jrpriv->jrdev = dev;

	INIT_LIST_HEAD(&dma_jr->done_not_acked);
	spin_lock_init(&dma_jr->done_lock);

	return 0;
}

static inline void caam_jr_chan_unbind(struct device *ctrldev,
				       struct dma_chan *chan)
{
	list_del(&chan->device_node);
}

static inline void caam_jr_free(struct dma_chan *chan)
{
	struct caam_dma_jr *dma_jr = container_of(chan, struct caam_dma_jr,
						  chan);

	list_del(&chan->device_node);
	kfree(dma_jr);
}

static int caam_jr_dma_init(struct device *ctrldev)
{
	struct caam_drv_private *priv = dev_get_drvdata(ctrldev);
	struct dma_device *dma_dev = NULL;
	struct caam_xor_sh_desc *sh_desc;
	int i;

	priv->xor_sh_desc =
	    kzalloc(sizeof(struct caam_xor_sh_desc), GFP_KERNEL | GFP_DMA);
	if (!priv->xor_sh_desc) {
		dev_err(ctrldev,
			"cannot alloc dma mem for XOR Shared desc\n");
		return -ENOMEM;
	}

	sh_desc = priv->xor_sh_desc;
	prepare_caam_xor_sh_desc(sh_desc->desc, MAX_XOR_SRCS);
	sh_desc->sh_desc_phys = dma_map_single(ctrldev, &sh_desc->desc,
						SH_DESC_LEN * sizeof(u32),
						DMA_TO_DEVICE);

	dma_dev = &priv->dma_dev;
	dma_dev->dev = ctrldev;
	INIT_LIST_HEAD(&dma_dev->channels);

	dma_dev->max_xor = MAX_XOR_SRCS;

	/*
	 * xor transaction must be 128 bytes aligned. For unaligned
	 * transaction, xor-parity calculations will not be off-loaded
	 * to caam
	 */
	dma_dev->xor_align = 8;
	dma_cap_set(DMA_XOR, dma_dev->cap_mask);

	dma_dev->device_alloc_chan_resources = caam_jr_alloc_chan_resources;
	dma_dev->device_tx_status = caam_jr_tx_status;
	dma_dev->device_issue_pending = caam_jr_issue_pending;
	dma_dev->device_prep_dma_xor = caam_jr_prep_dma_xor;
	dma_dev->device_free_chan_resources = caam_jr_free_chan_resources;

	for (i = 0; i < priv->total_jobrs; i++)
		caam_jr_chan_bind(ctrldev, priv->jrdev[i]);

	dma_async_device_register(dma_dev);
	dev_info(ctrldev, "caam xor support with %d job rings\n",
		 priv->total_jobrs);

	return 0;
}

static void caam_jr_dma_exit(struct device *ctrldev)
{
	struct caam_drv_private *priv = dev_get_drvdata(ctrldev);
	struct dma_device *dma_dev = &priv->dma_dev;
	struct dma_chan *chan, *_chan;
	struct list_head to_free;
	int i = 0;

	INIT_LIST_HEAD(&to_free);
	/* before unregistering device, remove channels... */
	list_for_each_entry_safe(chan, _chan, &dma_dev->channels, device_node) {
		caam_jr_chan_unbind(ctrldev, chan);
		list_add_tail(&chan->device_node, &to_free);
		i++;
	}

	dma_async_device_unregister(dma_dev);

	/*
	 * ...but don't delete them until device has been unregistered, so
	 * that deleted channels will not be used
	 */
	list_for_each_entry_safe(chan, _chan, &to_free, device_node) {
		caam_jr_free(chan);
	}

	dma_unmap_single(ctrldev, priv->xor_sh_desc[0].sh_desc_phys,
			 SH_DESC_LEN * sizeof(u32), DMA_TO_DEVICE);

	kfree(priv->xor_sh_desc);
	dev_info(ctrldev, "caam xor support disabled\n");
}

static int __init caam_xor_init(void)
{
	struct device_node *dev_node;
	struct platform_device *pdev;
	struct device *ctrldev;
	struct caam_drv_private *priv;
	int err = 0;

	dev_node = of_find_compatible_node(NULL, NULL, "fsl,sec-v4.0");
	if (!dev_node)
		return -ENODEV;

	pdev = of_find_device_by_node(dev_node);
	if (!pdev)
		return -ENODEV;

	ctrldev = &pdev->dev;
	priv = dev_get_drvdata(ctrldev);
	of_node_put(dev_node);

	atomic_set(&priv->tfm_count, -1);

	/* register caam device */
	err = caam_jr_dma_init(ctrldev);
	if (err)
		dev_err(ctrldev, "error in xor initialization: %d\n", err);

	return err;
}

static void __exit caam_xor_exit(void)
{
	struct device_node *dev_node;
	struct platform_device *pdev;
	struct device *ctrldev;
	struct caam_drv_private *priv;

	dev_node = of_find_compatible_node(NULL, NULL, "fsl,sec-v4.0");
	if (!dev_node)
		return;

	pdev = of_find_device_by_node(dev_node);
	if (!pdev)
		return;

	ctrldev = &pdev->dev;
	of_node_put(dev_node);
	priv = dev_get_drvdata(ctrldev);

	caam_jr_dma_exit(ctrldev);
}

module_init(caam_xor_init);
module_exit(caam_xor_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("FSL XOR offloading support by CAAM");
MODULE_AUTHOR("Naveen Burmi <naveenburmi@freescale.com>");
