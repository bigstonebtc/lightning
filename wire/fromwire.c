#include "wire.h"
#include <bitcoin/preimage.h>
#include <bitcoin/pubkey.h>
#include <bitcoin/shadouble.h>
#include <bitcoin/tx.h>
#include <ccan/build_assert/build_assert.h>
#include <ccan/endian/endian.h>
#include <ccan/mem/mem.h>
#include <ccan/tal/str/str.h>
#include <common/type_to_string.h>
#include <common/utils.h>

/* Sets *cursor to NULL and returns NULL when extraction fails. */
const void *fromwire_fail(const u8 **cursor, size_t *max)
{
	*cursor = NULL;
	*max = 0;
	return NULL;
}

const u8 *fromwire(const u8 **cursor, size_t *max, void *copy, size_t n)
{
	const u8 *p = *cursor;

	if (*max < n) {
		/* Just make sure we don't leak uninitialized mem! */
		if (copy)
			memset(copy, 0, n);
		return fromwire_fail(cursor, max);
	}
	*cursor += n;
	*max -= n;
	if (copy)
		memcpy(copy, p, n);
	return memcheck(p, n);
}

int fromwire_peektype(const u8 *cursor)
{
	be16 be_type;
	size_t max = tal_count(cursor);

	fromwire(&cursor, &max, &be_type, sizeof(be_type));
	if (!cursor)
		return -1;
	return be16_to_cpu(be_type);
}

u8 fromwire_u8(const u8 **cursor, size_t *max)
{
	u8 ret;

	if (!fromwire(cursor, max, &ret, sizeof(ret)))
		return 0;
	return ret;
}

u16 fromwire_u16(const u8 **cursor, size_t *max)
{
	be16 ret;

	if (!fromwire(cursor, max, &ret, sizeof(ret)))
		return 0;
	return be16_to_cpu(ret);
}

u32 fromwire_u32(const u8 **cursor, size_t *max)
{
	be32 ret;

	if (!fromwire(cursor, max, &ret, sizeof(ret)))
		return 0;
	return be32_to_cpu(ret);
}

u64 fromwire_u64(const u8 **cursor, size_t *max)
{
	be64 ret;

	if (!fromwire(cursor, max, &ret, sizeof(ret)))
		return 0;
	return be64_to_cpu(ret);
}

bool fromwire_bool(const u8 **cursor, size_t *max)
{
	u8 ret;

	if (!fromwire(cursor, max, &ret, sizeof(ret)))
		return false;
	if (ret != 0 && ret != 1)
		fromwire_fail(cursor, max);
	return ret;
}

void fromwire_pubkey(const u8 **cursor, size_t *max, struct pubkey *pubkey)
{
	u8 der[PUBKEY_DER_LEN];

	if (!fromwire(cursor, max, der, sizeof(der)))
		return;

	if (!pubkey_from_der(der, sizeof(der), pubkey))
		fromwire_fail(cursor, max);
}

void fromwire_secret(const u8 **cursor, size_t *max, struct secret *secret)
{
	fromwire(cursor, max, secret->data, sizeof(secret->data));
}

void fromwire_privkey(const u8 **cursor, size_t *max, struct privkey *privkey)
{
	fromwire_secret(cursor, max, &privkey->secret);
}

void fromwire_secp256k1_ecdsa_signature(const u8 **cursor,
				size_t *max, secp256k1_ecdsa_signature *sig)
{
	u8 compact[64];

	if (!fromwire(cursor, max, compact, sizeof(compact)))
		return;

	if (secp256k1_ecdsa_signature_parse_compact(secp256k1_ctx, sig, compact)
	    != 1)
		fromwire_fail(cursor, max);
}

void fromwire_secp256k1_ecdsa_recoverable_signature(const u8 **cursor,
				    size_t *max,
				    secp256k1_ecdsa_recoverable_signature *rsig)
{
	u8 compact[64];
	int recid;

	fromwire(cursor, max, compact, sizeof(compact));
	recid = fromwire_u8(cursor, max);

	if (secp256k1_ecdsa_recoverable_signature_parse_compact(secp256k1_ctx,
								rsig, compact,
								recid) != 1)
		fromwire_fail(cursor, max);
}

void fromwire_channel_id(const u8 **cursor, size_t *max,
			 struct channel_id *channel_id)
{
	fromwire(cursor, max, channel_id, sizeof(*channel_id));
}

void fromwire_short_channel_id(const u8 **cursor, size_t *max,
			       struct short_channel_id *short_channel_id)
{
	be32 txnum = 0, blocknum = 0;

	/* Pulling 3 bytes off wire is tricky; they're big-endian. */
	fromwire(cursor, max, (char *)&blocknum + 1, 3);
	short_channel_id->blocknum = be32_to_cpu(blocknum);
	fromwire(cursor, max, (char *)&txnum + 1, 3);
	short_channel_id->txnum = be32_to_cpu(txnum);

	short_channel_id->outnum = fromwire_u16 (cursor, max);
}

void fromwire_sha256(const u8 **cursor, size_t *max, struct sha256 *sha256)
{
	fromwire(cursor, max, sha256, sizeof(*sha256));
}

void fromwire_sha256_double(const u8 **cursor, size_t *max,
			    struct sha256_double *sha256d)
{
	fromwire_sha256(cursor, max, &sha256d->sha);
}

void fromwire_bitcoin_txid(const u8 **cursor, size_t *max,
			   struct bitcoin_txid *txid)
{
	fromwire_sha256_double(cursor, max, &txid->shad);
}

void fromwire_bitcoin_blkid(const u8 **cursor, size_t *max,
			    struct bitcoin_blkid *blkid)
{
	fromwire_sha256_double(cursor, max, &blkid->shad);
}

void fromwire_preimage(const u8 **cursor, size_t *max, struct preimage *preimage)
{
	fromwire(cursor, max, preimage, sizeof(*preimage));
}

void fromwire_ripemd160(const u8 **cursor, size_t *max, struct ripemd160 *ripemd)
{
	fromwire(cursor, max, ripemd, sizeof(*ripemd));
}

void fromwire_u8_array(const u8 **cursor, size_t *max, u8 *arr, size_t num)
{
	fromwire(cursor, max, arr, num);
}

void fromwire_pad(const u8 **cursor, size_t *max, size_t num)
{
	fromwire(cursor, max, NULL, num);
}

REGISTER_TYPE_TO_STRING(short_channel_id, short_channel_id_to_str);
REGISTER_TYPE_TO_HEXSTR(channel_id);

/* BOLT #2:
 *
 * This message introduces the `channel_id` to identify the channel, which is
 * derived from the funding transaction by combining the `funding_txid` and
 * the `funding_output_index` using big-endian exclusive-OR
 * (ie. `funding_output_index` alters the last two bytes).
 */
void derive_channel_id(struct channel_id *channel_id,
		       struct bitcoin_txid *txid, u16 txout)
{
	BUILD_ASSERT(sizeof(*channel_id) == sizeof(*txid));
	memcpy(channel_id, txid, sizeof(*channel_id));
	channel_id->id[sizeof(*channel_id)-2] ^= txout >> 8;
	channel_id->id[sizeof(*channel_id)-1] ^= txout;
}

void fromwire_bitcoin_tx(const u8 **cursor, size_t *max, struct bitcoin_tx *tx)
{
	pull_bitcoin_tx_onto(tx, cursor, max, tx);
}
