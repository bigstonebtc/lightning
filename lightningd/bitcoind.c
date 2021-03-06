/* Code for talking to bitcoind.  We use bitcoin-cli. */
#include "bitcoin/base58.h"
#include "bitcoin/block.h"
#include "bitcoin/shadouble.h"
#include "bitcoin/tx.h"
#include "bitcoind.h"
#include "lightningd.h"
#include "log.h"
#include <ccan/cast/cast.h>
#include <ccan/io/io.h>
#include <ccan/pipecmd/pipecmd.h>
#include <ccan/str/hex/hex.h>
#include <ccan/take/take.h>
#include <ccan/tal/grab_file/grab_file.h>
#include <ccan/tal/path/path.h>
#include <ccan/tal/str/str.h>
#include <ccan/tal/tal.h>
#include <common/json.h>
#include <common/memleak.h>
#include <common/utils.h>
#include <errno.h>
#include <inttypes.h>
#include <lightningd/chaintopology.h>

#define BITCOIN_CLI "bitcoin-cli"

char *bitcoin_datadir;

static char **gather_args(const struct bitcoind *bitcoind,
			  const tal_t *ctx, const char *cmd, va_list ap)
{
	size_t n = 0;
	char **args = tal_arr(ctx, char *, 2);

	args[n++] = cast_const(char *, bitcoind->chainparams->cli);
	if (bitcoind->chainparams->cli_args) {
		args[n++] = cast_const(char *, bitcoind->chainparams->cli_args);
		tal_resize(&args, n + 1);
	}

	if (bitcoind->datadir) {
		args[n++] = tal_fmt(args, "-datadir=%s", bitcoind->datadir);
		tal_resize(&args, n + 1);
	}
	args[n++] = cast_const(char *, cmd);
	tal_resize(&args, n + 1);

	while ((args[n] = va_arg(ap, char *)) != NULL) {
		args[n] = tal_strdup(args, args[n]);
		n++;
		tal_resize(&args, n + 1);
	}
	return args;
}

struct bitcoin_cli {
	struct list_node list;
	struct bitcoind *bitcoind;
	int fd;
	int *exitstatus;
	pid_t pid;
	char **args;
	char *output;
	size_t output_bytes;
	size_t new_output;
	void (*process)(struct bitcoin_cli *);
	void *cb;
	void *cb_arg;
	struct bitcoin_cli **stopper;
};

static struct io_plan *read_more(struct io_conn *conn, struct bitcoin_cli *bcli)
{
	bcli->output_bytes += bcli->new_output;
	if (bcli->output_bytes == tal_count(bcli->output))
		tal_resize(&bcli->output, bcli->output_bytes * 2);
	return io_read_partial(conn, bcli->output + bcli->output_bytes,
			       tal_count(bcli->output) - bcli->output_bytes,
			       &bcli->new_output, read_more, bcli);
}

static struct io_plan *output_init(struct io_conn *conn, struct bitcoin_cli *bcli)
{
	bcli->output_bytes = bcli->new_output = 0;
	bcli->output = tal_arr(bcli, char, 100);
	return read_more(conn, bcli);
}

static void next_bcli(struct bitcoind *bitcoind);

/* For printing: simple string of args. */
static char *bcli_args(struct bitcoin_cli *bcli)
{
	size_t i;
	char *ret = tal_strdup(bcli, bcli->args[0]);

	for (i = 1; bcli->args[i]; i++) {
		ret = tal_strcat(bcli, take(ret), " ");
		ret = tal_strcat(bcli, take(ret), bcli->args[i]);
	}
	return ret;
}

static void bcli_finished(struct io_conn *conn, struct bitcoin_cli *bcli)
{
	int ret, status;
	struct bitcoind *bitcoind = bcli->bitcoind;

	/* FIXME: If we waited for SIGCHILD, this could never hang! */
	ret = waitpid(bcli->pid, &status, 0);
	if (ret != bcli->pid)
		fatal("%s %s", bcli_args(bcli),
		      ret == 0 ? "not exited?" : strerror(errno));

	if (!WIFEXITED(status))
		fatal("%s died with signal %i",
		      bcli_args(bcli),
		      WTERMSIG(status));

	if (!bcli->exitstatus) {
		if (WEXITSTATUS(status) != 0) {
			/* Allow 60 seconds of spurious errors, eg. reorg. */
			struct timerel t;

			log_unusual(bcli->bitcoind->log,
				    "%s exited with status %u",
				    bcli_args(bcli),
				    WEXITSTATUS(status));

			if (!bitcoind->error_count)
				bitcoind->first_error_time = time_mono();

			t = timemono_between(time_mono(),
					     bitcoind->first_error_time);
			if (time_greater(t, time_from_sec(60)))
				fatal("%s exited %u (after %u other errors) '%.*s'",
				      bcli_args(bcli),
				      WEXITSTATUS(status),
				      bitcoind->error_count,
				      (int)bcli->output_bytes,
				      bcli->output);
			bitcoind->error_count++;
		}
	} else
		*bcli->exitstatus = WEXITSTATUS(status);

	if (WEXITSTATUS(status) == 0)
		bitcoind->error_count = 0;

	bitcoind->req_running = false;

	/* Don't continue if were only here because we were freed for shutdown */
	if (bitcoind->shutdown)
		return;

	db_begin_transaction(bitcoind->ld->wallet->db);
	bcli->process(bcli);
	db_commit_transaction(bitcoind->ld->wallet->db);
	tal_free(bcli);

	next_bcli(bitcoind);
}

static void next_bcli(struct bitcoind *bitcoind)
{
	struct bitcoin_cli *bcli;
	struct io_conn *conn;

	if (bitcoind->req_running)
		return;

	bcli = list_pop(&bitcoind->pending, struct bitcoin_cli, list);
	if (!bcli)
		return;

	bcli->pid = pipecmdarr(&bcli->fd, NULL, &bcli->fd, bcli->args);
	if (bcli->pid < 0)
		fatal("%s exec failed: %s", bcli->args[0], strerror(errno));

	bitcoind->req_running = true;
	/* This lifetime is attached to bitcoind command fd */
	conn = notleak(io_new_conn(bitcoind, bcli->fd, output_init, bcli));
	io_set_finish(conn, bcli_finished, bcli);
}

static void process_donothing(struct bitcoin_cli *bcli)
{
}

/* If stopper gets freed first, set process() to a noop. */
static void stop_process_bcli(struct bitcoin_cli **stopper)
{
	(*stopper)->process = process_donothing;
	(*stopper)->stopper = NULL;
}

/* It command finishes first, free stopper. */
static void remove_stopper(struct bitcoin_cli *bcli)
{
	/* Calls stop_process_bcli, but we don't care. */
	tal_free(bcli->stopper);
}

/* If ctx is non-NULL, and is freed before we return, we don't call process() */
static void
start_bitcoin_cli(struct bitcoind *bitcoind,
		  const tal_t *ctx,
		  void (*process)(struct bitcoin_cli *),
		  bool nonzero_exit_ok,
		  void *cb, void *cb_arg,
		  char *cmd, ...)
{
	va_list ap;
	struct bitcoin_cli *bcli = tal(bitcoind, struct bitcoin_cli);

	bcli->bitcoind = bitcoind;
	bcli->process = process;
	bcli->cb = cb;
	bcli->cb_arg = cb_arg;
	if (ctx) {
		/* Create child whose destructor will stop us calling */
		bcli->stopper = tal(ctx, struct bitcoin_cli *);
		*bcli->stopper = bcli;
		tal_add_destructor(bcli->stopper, stop_process_bcli);
		tal_add_destructor(bcli, remove_stopper);
	} else
		bcli->stopper = NULL;

	if (nonzero_exit_ok)
		bcli->exitstatus = tal(bcli, int);
	else
		bcli->exitstatus = NULL;
	va_start(ap, cmd);
	bcli->args = gather_args(bitcoind, bcli, cmd, ap);
	va_end(ap);

	list_add_tail(&bitcoind->pending, &bcli->list);
	next_bcli(bitcoind);
}

static bool extract_feerate(struct bitcoin_cli *bcli,
			    const char *output, size_t output_bytes,
			    double *feerate)
{
	const jsmntok_t *tokens, *feeratetok;
	bool valid;

	tokens = json_parse_input(output, output_bytes, &valid);
	if (!tokens)
		fatal("%s: %s response",
		      bcli_args(bcli),
		      valid ? "partial" : "invalid");

	if (tokens[0].type != JSMN_OBJECT)
		fatal("%s: gave non-object (%.*s)?",
		      bcli_args(bcli),
		      (int)output_bytes, output);

	feeratetok = json_get_member(output, tokens, "feerate");
	if (!feeratetok)
		return false;

	return json_tok_double(output, feeratetok, feerate);
}

struct estimatefee {
	size_t i;
	const u32 *blocks;
	const char **estmode;

	void (*cb)(struct bitcoind *bitcoind, const u32 satoshi_per_kw[],
		   void *);
	void *arg;
	u32 *satoshi_per_kw;
};

static void do_one_estimatefee(struct bitcoind *bitcoind,
			       struct estimatefee *efee);

static void process_estimatefee(struct bitcoin_cli *bcli)
{
	double feerate;
	struct estimatefee *efee = bcli->cb_arg;

	/* FIXME: We could trawl recent blocks for median fee... */
	if (!extract_feerate(bcli, bcli->output, bcli->output_bytes, &feerate)) {
		log_unusual(bcli->bitcoind->log, "Unable to estimate %s/%u fee",
			    efee->estmode[efee->i], efee->blocks[efee->i]);
		efee->satoshi_per_kw[efee->i] = 0;
	} else
		/* Rate in satoshi per kw. */
		efee->satoshi_per_kw[efee->i] = feerate * 100000000 / 4;

	efee->i++;
	if (efee->i == tal_count(efee->satoshi_per_kw)) {
		efee->cb(bcli->bitcoind, efee->satoshi_per_kw, efee->arg);
		tal_free(efee);
	} else {
		/* Next */
		do_one_estimatefee(bcli->bitcoind, efee);
	}
}

static void do_one_estimatefee(struct bitcoind *bitcoind,
			       struct estimatefee *efee)
{
	char blockstr[STR_MAX_CHARS(u32)];

	sprintf(blockstr, "%u", efee->blocks[efee->i]);
	start_bitcoin_cli(bitcoind, NULL, process_estimatefee, false, NULL, efee,
			  "estimatesmartfee", blockstr, efee->estmode[efee->i],
			  NULL);
}

void bitcoind_estimate_fees_(struct bitcoind *bitcoind,
			     const u32 blocks[], const char *estmode[],
			     size_t num_estimates,
			     void (*cb)(struct bitcoind *bitcoind,
					const u32 satoshi_per_kw[], void *),
			     void *arg)
{
	struct estimatefee *efee = tal(bitcoind, struct estimatefee);

	efee->i = 0;
	efee->blocks = tal_dup_arr(efee, u32, blocks, num_estimates, 0);
	efee->estmode = tal_dup_arr(efee, const char *, estmode, num_estimates,
				    0);
	efee->cb = cb;
	efee->arg = arg;
	efee->satoshi_per_kw = tal_arr(efee, u32, num_estimates);

	do_one_estimatefee(bitcoind, efee);
}

static void process_sendrawtx(struct bitcoin_cli *bcli)
{
	void (*cb)(struct bitcoind *bitcoind,
		   int, const char *msg, void *) = bcli->cb;
	const char *msg = tal_strndup(bcli, (char *)bcli->output,
				      bcli->output_bytes);

	log_debug(bcli->bitcoind->log, "sendrawtx exit %u, gave %s",
		  *bcli->exitstatus, msg);

	cb(bcli->bitcoind, *bcli->exitstatus, msg, bcli->cb_arg);
}

void bitcoind_sendrawtx_(struct bitcoind *bitcoind,
			 const char *hextx,
			 void (*cb)(struct bitcoind *bitcoind,
				    int exitstatus, const char *msg, void *),
			 void *arg)
{
	log_debug(bitcoind->log, "sendrawtransaction: %s", hextx);
	start_bitcoin_cli(bitcoind, NULL, process_sendrawtx, true, cb, arg,
			  "sendrawtransaction", hextx, NULL);
}

static void process_rawblock(struct bitcoin_cli *bcli)
{
	struct bitcoin_block *blk;
	void (*cb)(struct bitcoind *bitcoind,
		   struct bitcoin_block *blk,
		   void *arg) = bcli->cb;

	/* FIXME: Just get header if we can't get full block. */
	blk = bitcoin_block_from_hex(bcli, bcli->output, bcli->output_bytes);
	if (!blk)
		fatal("%s: bad block '%.*s'?",
		      bcli_args(bcli),
		      (int)bcli->output_bytes, (char *)bcli->output);

	cb(bcli->bitcoind, blk, bcli->cb_arg);
}

void bitcoind_getrawblock_(struct bitcoind *bitcoind,
			   const struct bitcoin_blkid *blockid,
			   void (*cb)(struct bitcoind *bitcoind,
				      struct bitcoin_block *blk,
				      void *arg),
			   void *arg)
{
	char hex[hex_str_size(sizeof(*blockid))];

	bitcoin_blkid_to_hex(blockid, hex, sizeof(hex));
	start_bitcoin_cli(bitcoind, NULL, process_rawblock, false, cb, arg,
			  "getblock", hex, "false", NULL);
}

static void process_getblockcount(struct bitcoin_cli *bcli)
{
	u32 blockcount;
	char *p, *end;
	void (*cb)(struct bitcoind *bitcoind,
		   u32 blockcount,
		   void *arg) = bcli->cb;

	p = tal_strndup(bcli, bcli->output, bcli->output_bytes);
	blockcount = strtol(p, &end, 10);
	if (end == p || *end != '\n')
		fatal("%s: gave non-numeric blockcount %s",
		      bcli_args(bcli), p);

	cb(bcli->bitcoind, blockcount, bcli->cb_arg);
}

void bitcoind_getblockcount_(struct bitcoind *bitcoind,
			      void (*cb)(struct bitcoind *bitcoind,
					 u32 blockcount,
					 void *arg),
			      void *arg)
{
	start_bitcoin_cli(bitcoind, NULL, process_getblockcount, false, cb, arg,
			  "getblockcount", NULL);
}

struct get_output {
	unsigned int blocknum, txnum, outnum;

	/* The real callback arg */
	void *cbarg;
};

static void process_gettxout(struct bitcoin_cli *bcli)
{
	void (*cb)(struct bitcoind *bitcoind,
		   const struct bitcoin_tx_output *output,
		   void *arg) = bcli->cb;
	const struct get_output *go = bcli->cb_arg;
	void *cbarg = go->cbarg;
	const jsmntok_t *tokens, *valuetok, *scriptpubkeytok, *hextok;
	struct bitcoin_tx_output out;
	bool valid;

	if (*bcli->exitstatus != 0) {
		log_debug(bcli->bitcoind->log, "%s: not unspent output?",
			  bcli_args(bcli));
		tal_free(go);
		cb(bcli->bitcoind, NULL, cbarg);
		return;
	}

	tokens = json_parse_input(bcli->output, bcli->output_bytes, &valid);
	if (!tokens)
		fatal("%s: %s response",
		      bcli_args(bcli), valid ? "partial" : "invalid");

	if (tokens[0].type != JSMN_OBJECT)
		fatal("%s: gave non-object (%.*s)?",
		      bcli_args(bcli), (int)bcli->output_bytes, bcli->output);

	valuetok = json_get_member(bcli->output, tokens, "value");
	if (!valuetok)
		fatal("%s: had no value member (%.*s)?",
		      bcli_args(bcli), (int)bcli->output_bytes, bcli->output);

	if (!json_tok_bitcoin_amount(bcli->output, valuetok, &out.amount))
		fatal("%s: had bad value (%.*s)?",
		      bcli_args(bcli), (int)bcli->output_bytes, bcli->output);

	scriptpubkeytok = json_get_member(bcli->output, tokens, "scriptPubKey");
	if (!scriptpubkeytok)
		fatal("%s: had no scriptPubKey member (%.*s)?",
		      bcli_args(bcli), (int)bcli->output_bytes, bcli->output);
	hextok = json_get_member(bcli->output, scriptpubkeytok, "hex");
	if (!hextok)
		fatal("%s: had no scriptPubKey->hex member (%.*s)?",
		      bcli_args(bcli), (int)bcli->output_bytes, bcli->output);

	out.script = tal_hexdata(bcli, bcli->output + hextok->start,
				 hextok->end - hextok->start);
	if (!out.script)
		fatal("%s: scriptPubKey->hex invalid hex (%.*s)?",
		      bcli_args(bcli), (int)bcli->output_bytes, bcli->output);

	tal_free(go);
	cb(bcli->bitcoind, &out, cbarg);
}

static void process_getblock(struct bitcoin_cli *bcli)
{
	void (*cb)(struct bitcoind *bitcoind,
		   const struct bitcoin_tx_output *output,
		   void *arg) = bcli->cb;
	struct get_output *go = bcli->cb_arg;
	const jsmntok_t *tokens, *txstok, *txidtok;
	struct bitcoin_txid txid;
	bool valid;

	tokens = json_parse_input(bcli->output, bcli->output_bytes, &valid);
	if (!tokens)
		fatal("%s: %s response",
		      bcli_args(bcli), valid ? "partial" : "invalid");

	if (tokens[0].type != JSMN_OBJECT)
		fatal("%s: gave non-object (%.*s)?",
		      bcli_args(bcli), (int)bcli->output_bytes, bcli->output);

	/*  "tx": [
	    "1a7bb0f58a5d235d232deb61d9e2208dabe69848883677abe78e9291a00638e8",
	    "56a7e3468c16a4e21a4722370b41f522ad9dd8006c0e4e73c7d1c47f80eced94",
	    ...
	*/
	txstok = json_get_member(bcli->output, tokens, "tx");
	if (!txstok)
		fatal("%s: had no tx member (%.*s)?",
		      bcli_args(bcli), (int)bcli->output_bytes, bcli->output);

	/* Now, this can certainly happen, if txnum too large. */
	txidtok = json_get_arr(txstok, go->txnum);
	if (!txidtok) {
		void *cbarg = go->cbarg;
		log_debug(bcli->bitcoind->log, "%s: no txnum %u",
			  bcli_args(bcli), go->txnum);
		tal_free(go);
		cb(bcli->bitcoind, NULL, cbarg);
		return;
	}

	if (!bitcoin_txid_from_hex(bcli->output + txidtok->start,
				   txidtok->end - txidtok->start,
				   &txid))
		fatal("%s: had bad txid (%.*s)?",
		      bcli_args(bcli),
		      txidtok->end - txidtok->start,
		      bcli->output + txidtok->start);

	/* Now get the raw tx output. */
	start_bitcoin_cli(bcli->bitcoind, NULL,
			  process_gettxout, true, cb, go,
			  "gettxout",
			  take(type_to_string(go, struct bitcoin_txid, &txid)),
			  take(tal_fmt(go, "%u", go->outnum)),
			  NULL);
}

static void process_getblockhash_for_txout(struct bitcoin_cli *bcli)
{
	void (*cb)(struct bitcoind *bitcoind,
		   const struct bitcoin_tx_output *output,
		   void *arg) = bcli->cb;
	struct get_output *go = bcli->cb_arg;

	if (*bcli->exitstatus != 0) {
		void *cbarg = go->cbarg;
		log_debug(bcli->bitcoind->log, "%s: invalid blocknum?",
			  bcli_args(bcli));
		tal_free(go);
		cb(bcli->bitcoind, NULL, cbarg);
		return;
	}

	start_bitcoin_cli(bcli->bitcoind, NULL, process_getblock, false, cb, go,
			  "getblock",
			  take(tal_strndup(go, bcli->output,bcli->output_bytes)),
			  NULL);
}

void bitcoind_getoutput_(struct bitcoind *bitcoind,
			 unsigned int blocknum, unsigned int txnum,
			 unsigned int outnum,
			 void (*cb)(struct bitcoind *bitcoind,
				    const struct bitcoin_tx_output *output,
				    void *arg),
			 void *arg)
{
	struct get_output *go = tal(bitcoind, struct get_output);
	go->blocknum = blocknum;
	go->txnum = txnum;
	go->outnum = outnum;
	go->cbarg = arg;

	/* We may not have topology ourselves that far back, so ask bitcoind */
	start_bitcoin_cli(bitcoind, NULL, process_getblockhash_for_txout,
			  true, cb, go,
			  "getblockhash", take(tal_fmt(go, "%u", blocknum)),
			  NULL);

	/* Looks like a leak, but we free it in process_getblock */
	notleak(go);
}

static void process_getblockhash(struct bitcoin_cli *bcli)
{
	struct bitcoin_blkid blkid;
	void (*cb)(struct bitcoind *bitcoind,
		   const struct bitcoin_blkid *blkid,
		   void *arg) = bcli->cb;

	/* If it failed, call with NULL block. */
	if (*bcli->exitstatus != 0) {
		cb(bcli->bitcoind, NULL, bcli->cb_arg);
		return;
	}

	if (bcli->output_bytes == 0
	    || !bitcoin_blkid_from_hex(bcli->output, bcli->output_bytes-1,
				       &blkid)) {
		fatal("%s: bad blockid '%.*s'",
		      bcli_args(bcli), (int)bcli->output_bytes, bcli->output);
	}

	cb(bcli->bitcoind, &blkid, bcli->cb_arg);
}

void bitcoind_getblockhash_(struct bitcoind *bitcoind,
			    u32 height,
			    void (*cb)(struct bitcoind *bitcoind,
				       const struct bitcoin_blkid *blkid,
				       void *arg),
			    void *arg)
{
	char str[STR_MAX_CHARS(height)];
	sprintf(str, "%u", height);

	start_bitcoin_cli(bitcoind, NULL, process_getblockhash, true, cb, arg,
			  "getblockhash", str, NULL);
}

static void destroy_bitcoind(struct bitcoind *bitcoind)
{
	/* Suppresses the callbacks from bcli_finished as we free conns. */
	bitcoind->shutdown = true;
}

static char **cmdarr(const tal_t *ctx, const struct bitcoind *bitcoind,
		     const char *cmd, ...)
{
	va_list ap;
	char **args;

	va_start(ap, cmd);
	args = gather_args(bitcoind, ctx, cmd, ap);
	va_end(ap);
	return args;
}

void wait_for_bitcoind(struct bitcoind *bitcoind)
{
	int from, ret, status;
	pid_t child;
	char **cmd = cmdarr(bitcoind, bitcoind, "echo", NULL);
	char *output;
	bool printed = false;

	for (;;) {
		child = pipecmdarr(&from, NULL, &from, cmd);
		if (child < 0)
			fatal("%s exec failed: %s", cmd[0], strerror(errno));

		output = grab_fd(cmd, from);
		if (!output)
			fatal("Reading from %s failed: %s",
			      cmd[0], strerror(errno));

		ret = waitpid(child, &status, 0);
		if (ret != child)
			fatal("Waiting for %s: %s", cmd[0], strerror(errno));
		if (!WIFEXITED(status))
			fatal("Death of %s: signal %i",
			      cmd[0], WTERMSIG(status));

		if (WEXITSTATUS(status) == 0)
			break;

		/* bitcoin/src/rpc/protocol.h:
		 *	RPC_IN_WARMUP = -28, //!< Client still warming up
		 */
		if (WEXITSTATUS(status) != 28)
			fatal("%s exited with code %i: %s",
			      cmd[0], WEXITSTATUS(status), output);

		if (!printed) {
			log_unusual(bitcoind->log,
				    "Waiting for bitcoind to warm up...");
			printed = true;
		}
		sleep(1);
	}
	tal_free(cmd);
}

struct bitcoind *new_bitcoind(const tal_t *ctx,
			      struct lightningd *ld,
			      struct log *log)
{
	struct bitcoind *bitcoind = tal(ctx, struct bitcoind);

	/* Use testnet by default, change later if we want another network */
	bitcoind->chainparams = chainparams_for_network("testnet");
	bitcoind->datadir = NULL;
	bitcoind->ld = ld;
	bitcoind->log = log;
	bitcoind->req_running = false;
	bitcoind->shutdown = false;
	bitcoind->error_count = 0;
	list_head_init(&bitcoind->pending);
	tal_add_destructor(bitcoind, destroy_bitcoind);

	return bitcoind;
}
