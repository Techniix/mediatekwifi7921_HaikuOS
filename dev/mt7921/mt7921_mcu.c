/*
 * Copyright 2026, Techniix. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Portions derived from MediaTek's mt76 driver (mcu.c, mt76_connac_mcu.c,
 * mt76_connac_mcu.h, mt792x_core.c, mt7921/mcu.c, mt7921/pci_mcu.c):
 *   Copyright (C) 2019 Lorenzo Bianconi <lorenzo.bianconi83@gmail.com>
 *   Copyright (C) 2020-2023 MediaTek Inc.
 *   SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * MCU messaging and firmware download for MT7921/MT7922 ("connac2").
 * Everything is synchronous and polled: send a command on the WM ring,
 * then poll both MCU event rings for the reply with the matching sequence
 * number.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/firmware.h>
#include <sys/malloc.h>

#include <dev/mt7921/if_mt7921var.h>
#include <dev/mt7921/mt7921_reg.h>


/*
 * Command encoding, as in mt76_connac_mcu.h: bits 7:0 command ID, 15:8
 * extended ID, plus family flags. mt7921_mcu_send() turns this into the
 * right TXD.
 */
#define MCU_F_EXT_SHIFT			8
#define MCU_F_QUERY			BIT(16)
#define MCU_F_UNI			BIT(17)
#define MCU_F_CE			BIT(18)
#define MCU_CMD_EXT_CID			0xed
#define MCU_EXT_CMD(id)			(MCU_CMD_EXT_CID | ((id) << MCU_F_EXT_SHIFT))
#define MCU_UNI_CMD(id)			(MCU_F_UNI | (id))
#define MCU_CE_CMD(id)			(MCU_F_CE | (id))

/* Command IDs (mt76_connac_mcu.h). */
#define MCU_CMD_TARGET_ADDRESS_LEN_REQ	0x01
#define MCU_CMD_FW_START_REQ		0x02
#define MCU_CMD_NIC_POWER_CTRL		0x04
#define MCU_CMD_PATCH_START_REQ		0x05
#define MCU_CMD_PATCH_FINISH_REQ	0x07
#define MCU_CMD_PATCH_SEM_CONTROL	0x10
#define MCU_CMD_FW_SCATTER		0xee
#define MCU_CE_CMD_GET_NIC_CAPAB	0x8a
#define MCU_CE_CMD_START_HW_SCAN	0x03
#define MCU_CE_CMD_SET_CHAN_DOMAIN	0x0f
#define MCU_CE_CMD_CANCEL_HW_SCAN	0x1b
#define MCU_EXT_CMD_EFUSE_BUFFER_MODE	0x21
#define MCU_EXT_CMD_MAC_INIT_CTRL	0x46
#define MCU_EXT_CMD_SET_RX_PATH		0x4e
#define MCU_UNI_CMD_DEV_INFO_UPDATE	0x01
#define MCU_UNI_CMD_BSS_INFO_UPDATE	0x02
#define MCU_UNI_CMD_STA_REC_UPDATE	0x03
#define MCU_UNI_CMD_ROC			0x27
#define MCU_UNI_EVENT_ROC		0x27

/* Unsolicited event IDs. */
#define MCU_EVENT_SCAN_DONE		0x0d

/* TXD for MCU commands (struct mt76_connac2_mcu_txd): 64 bytes. */
#define MCU_TXD_SIZE			64
#define MT_TXD0_PKT_FMT_SHIFT		23
#define MT_TXD0_Q_IDX_SHIFT		25
#define MT_TX_TYPE_CMD			2
#define MT_TX_MCU_PORT_RX_Q0		0x20
#define MT_TX_PORT_IDX_MCU		1
#define MT_TXD1_LONG_FORMAT		BIT(31)
#define MT_TXD1_HDR_FORMAT_SHIFT	16
#define MT_HDR_FORMAT_CMD		1
#define MCU_PQ_ID(p, q)			(((p) << 15) | ((q) << 10))
#define MCU_PKT_ID			0xa0
#define MCU_Q_QUERY			0
#define MCU_Q_SET			1
#define MCU_Q_NA			3
#define MCU_S2D_H2N			0
#define UNI_TXD_SIZE			48	/* struct mt76_connac2_mcu_uni_txd */
#define MCU_CMD_UNI_EXT_ACK		0x7	/* ACK | UNI | SET */

/* RXD for MCU events (struct mt76_connac2_mcu_rxd): 36-byte header. */
#define MCU_RXD_SIZE			36
#define MCU_RXD_EID			28
#define MCU_RXD_SEQ			29
#define MCU_RXD_OPTION			30
#define MCU_RXD_EXT_EID			32
#define MT_RXD0_PKT_TYPE_SHIFT		27
#define MT_RXD0_PKT_FLAG_SHIFT		16
#define PKT_TYPE_NORMAL			2
#define PKT_TYPE_TXRX_NOTIFY		6
#define PKT_TYPE_RX_EVENT		7
#define MCU_UNI_CMD_UNSOLICITED_EVENT	BIT(2)

/* Patch semaphore. */
#define PATCH_SEM_RELEASE		0
#define PATCH_SEM_GET			1
#define PATCH_NOT_DL_SEM_FAIL		0
#define PATCH_IS_DL			1
#define PATCH_NOT_DL_SEM_SUCCESS	2
#define PATCH_REL_SEM_SUCCESS		3

/* Download modes and feature bits. */
#define DL_MODE_ENCRYPT			BIT(0)
#define DL_MODE_KEY_IDX			0x6	/* GENMASK(2, 1) */
#define DL_MODE_RESET_SEC_IV		BIT(3)
#define DL_CONFIG_ENCRY_MODE_SEL	BIT(6)
#define DL_MODE_NEED_RSP		BIT(31)
#define FW_FEATURE_SET_ENCRYPT		BIT(0)
#define FW_FEATURE_SET_KEY_IDX		0x6	/* GENMASK(2, 1) */
#define FW_FEATURE_ENCRY_MODE		BIT(4)
#define FW_FEATURE_OVERRIDE_ADDR	BIT(5)
#define FW_FEATURE_NON_DL		BIT(6)
#define FW_START_OVERRIDE		BIT(0)
#define PATCH_SEC_NOT_SUPPORT		0xffffffffU
#define PATCH_SEC_TYPE_MASK		0xffffU
#define PATCH_SEC_TYPE_INFO		0x2
#define PATCH_SEC_ENC_TYPE_SHIFT	24
#define PATCH_SEC_ENC_TYPE_PLAIN	0x00
#define PATCH_SEC_ENC_TYPE_AES		0x01
#define PATCH_SEC_ENC_TYPE_SCRAMBLE	0x02
#define PATCH_SEC_ENC_AES_KEY_MASK	0xffU
#define MCU_PATCH_ADDRESS		0x200000

/* ROM patch file layout (big endian; struct mt76_connac2_patch_hdr/sec). */
#define PATCH_HDR_SIZE			96
#define PATCH_HDR_BUILD_DATE		0	/* char[16] */
#define PATCH_HDR_HW_SW_VER		20
#define PATCH_HDR_N_REGION		44
#define PATCH_SEC_SIZE			64
#define PATCH_SEC_TYPE			0
#define PATCH_SEC_OFFS			4
#define PATCH_SEC_ADDR			12
#define PATCH_SEC_LEN			16
#define PATCH_SEC_KEY_IDX		20

/* RAM firmware layout (little endian; mt76_connac2_fw_trailer/region). */
#define FW_TRAILER_SIZE			36
#define FW_TRAILER_N_REGION		2
#define FW_TRAILER_FW_VER		7	/* char[10] */
#define FW_TRAILER_BUILD_DATE		17	/* char[15] */
#define FW_REGION_SIZE			40
#define FW_REGION_ADDR			16
#define FW_REGION_LEN			20
#define FW_REGION_FEATURE_SET		24

/* NIC capability TLV types. */
#define MT_NIC_CAP_MAC_ADDR		7

#define MCU_TIMEOUT_MS			3000
#define FW_SCATTER_MAX_LEN		4096
#define MCU_MAX_PAYLOAD			(4096 - MCU_TXD_SIZE)
#define MCU_RESP_MAX			2048


/*
 * Persistent MCU state, one per device (sc->sc_mcu). Everything below is
 * protected by sc->sc_mtx; mt7921_mcu_send() must be called without it.
 */
struct mt7921_mcu {
	struct mt7921_softc*	sc;
	uint8_t*		txbuf;		/* TXD + payload */
	uint8_t*		resp;		/* last matching response */
	int			resp_len;
	uint8_t			wait_seq;	/* 0: nobody waiting */
	bool			got_resp;
	bool			busy;		/* a command is in flight */
};


static inline uint32_t
get_be32(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
		| ((uint32_t)p[2] << 8) | p[3];
}


static inline uint32_t
get_le32(const uint8_t* p)
{
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[1] << 8) | p[0];
}


static inline void
put_le16(uint8_t* p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}


static inline void
put_le32(uint8_t* p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = v >> 24;
}


/* #pragma mark - receive */


/*
 * Dispatch one received buffer from any RX ring. Port of
 * mt7921_queue_rx_skb() + mt7921_mcu_rx_event(). Called with sc_mtx held.
 */
static int
mt7921_mcu_rx(struct mt7921_softc* sc, const uint8_t* data, int len, void* arg)
{
	struct mt7921_mcu* mcu = sc->sc_mcu;
	uint32_t rxd0;
	int type, flag;
	uint8_t eid, seq, option;

	if (len < 4)
		return 0;

	rxd0 = get_le32(data);
	type = rxd0 >> MT_RXD0_PKT_TYPE_SHIFT;
	flag = (rxd0 >> MT_RXD0_PKT_FLAG_SHIFT) & 0xf;

	/* 802.11 frames: on the data ring, or forwarded by the MCU. */
	if (type == PKT_TYPE_NORMAL || (type == PKT_TYPE_RX_EVENT && flag == 0x1)) {
		mt7921_rx_frame(sc, data, len);
		return 0;
	}
	if (type == PKT_TYPE_TXRX_NOTIFY) {
		mt7921_tx_free_event(sc, data, len);
		return 0;
	}
	if (type != PKT_TYPE_RX_EVENT) {
		/* TX status etc.: not used. */
		return 0;
	}
	if (len < MCU_RXD_EXT_EID + 1)
		return 0;

	eid = data[MCU_RXD_EID];
	seq = data[MCU_RXD_SEQ];
	option = data[MCU_RXD_OPTION];

	if (mcu != NULL && (option & MCU_UNI_CMD_UNSOLICITED_EVENT) == 0
		&& seq != 0 && seq == mcu->wait_seq && !mcu->got_resp
		&& eid != MCU_EVENT_SCAN_DONE) {
		mcu->resp_len = MIN(len, MCU_RESP_MAX);
		memcpy(mcu->resp, data, mcu->resp_len);
		mcu->got_resp = true;
		wakeup(mcu);
		return 1;
	}

	if (eid == MCU_EVENT_SCAN_DONE) {
		mt7921_scan_done_event(sc);
		return 0;
	}
	if (eid == MCU_UNI_EVENT_ROC
		&& (option & MCU_UNI_CMD_UNSOLICITED_EVENT) != 0) {
		/* struct mt7921_roc_grant_tlv at rxd->tlv + 4; tag 0 = GRANT */
		if (len >= MCU_RXD_SIZE + 4 + 4 && data[MCU_RXD_SIZE + 4] == 0
			&& data[MCU_RXD_SIZE + 5] == 0)
			mt7921_roc_grant_event(sc);
		return 0;
	}

	if (sc->sc_mcu_evt_logged++ < 24) {
		device_printf(sc->sc_dev, "MCU event eid 0x%02x ext 0x%02x "
			"seq %u (ignored)\n", eid, data[MCU_RXD_EXT_EID], seq);
	}
	return 0;
}


/* Drain all RX rings. Called with sc_mtx held (interrupt thread, waits). */
void
mt7921_mcu_poll_locked(struct mt7921_softc* sc)
{
	mt7921_dma_rx_poll(sc, &sc->sc_rx_mcu, mt7921_mcu_rx, NULL);
	mt7921_dma_rx_poll(sc, &sc->sc_rx_wa, mt7921_mcu_rx, NULL);
	mt7921_dma_rx_poll(sc, &sc->sc_rx_data, mt7921_mcu_rx, NULL);
}


/* #pragma mark - send */


/*
 * Port of mt76_mcu_skb_send_and_get_msg() + mt76_connac2_mcu_fill_message()
 * + mt7921_mcu_send_message() + mt7921_mcu_parse_response().
 *
 * cmd uses the MCU_*_CMD() encoding above. Must be called without sc_mtx.
 * On success with wait set, the response is in mcu->resp / mcu->resp_len
 * (including the 36-byte RXD header) until the next command. Returns a
 * Haiku error code (negative) or the command's status (>= 0) as defined by
 * mt7921_mcu_parse_response().
 */
static int
mt7921_mcu_send(struct mt7921_mcu* mcu, uint32_t cmd, const void* payload,
	int plen, bool wait)
{
	struct mt7921_softc* sc = mcu->sc;
	uint8_t* txd = mcu->txbuf;
	uint8_t id = cmd & 0xff, ext = (cmd >> MCU_F_EXT_SHIFT) & 0xff;
	uint8_t seq;
	int total, hdr, error, deadline;

	MT7921_LOCK(sc);
	while (mcu->busy)
		msleep(&mcu->busy, &sc->sc_mtx, 0, "mt7921mcuq", 0);
	mcu->busy = true;

	seq = ++sc->sc_mcu_seq & 0xf;
	if (seq == 0)
		seq = ++sc->sc_mcu_seq & 0xf;

	if (cmd == MCU_CMD_FW_SCATTER) {
		/* Raw firmware data, no TXD, on the download ring. */
		error = mt7921_dma_tx(sc, &sc->sc_tx_fwdl, payload, plen);
		goto out;
	}

	hdr = (cmd & MCU_F_UNI) ? UNI_TXD_SIZE : MCU_TXD_SIZE;
	if (plen < 0 || plen > 4096 - hdr) {
		error = EINVAL;
		goto out;
	}

	total = hdr + plen;
	memset(txd, 0, hdr);
	put_le32(txd + 0, (uint32_t)(total & 0xffff)
		| ((uint32_t)MT_TX_TYPE_CMD << MT_TXD0_PKT_FMT_SHIFT)
		| ((uint32_t)MT_TX_MCU_PORT_RX_Q0 << MT_TXD0_Q_IDX_SHIFT));
	put_le32(txd + 4, MT_TXD1_LONG_FORMAT
		| ((uint32_t)MT_HDR_FORMAT_CMD << MT_TXD1_HDR_FORMAT_SHIFT));

	if (cmd & MCU_F_UNI) {
		/* struct mt76_connac2_mcu_uni_txd */
		put_le16(txd + 32, (uint16_t)(total - 32));	/* len */
		put_le16(txd + 34, id);				/* cid */
		txd[37] = MCU_PKT_ID;				/* pkt_type */
		txd[39] = seq;
		txd[42] = MCU_S2D_H2N;				/* s2d_index */
		txd[43] = MCU_CMD_UNI_EXT_ACK;			/* option */
	} else {
		/* struct mt76_connac2_mcu_txd */
		put_le16(txd + 32, (uint16_t)(total - 32));
		put_le16(txd + 34,
			(uint16_t)MCU_PQ_ID(MT_TX_PORT_IDX_MCU, MT_TX_MCU_PORT_RX_Q0));
		txd[36] = id;					/* cid */
		txd[37] = MCU_PKT_ID;
		if (ext != 0 || (cmd & MCU_F_CE))
			txd[38] = (cmd & MCU_F_QUERY) ? MCU_Q_QUERY : MCU_Q_SET;
		else
			txd[38] = MCU_Q_NA;
		txd[39] = seq;
		txd[41] = ext;					/* ext_cid */
		txd[42] = MCU_S2D_H2N;
		txd[43] = ext != 0 ? 1 : 0;			/* ext_cid_ack */
	}
	if (plen > 0)
		memcpy(txd + hdr, payload, plen);

	mcu->wait_seq = wait ? seq : 0;
	mcu->got_resp = false;
	mcu->resp_len = 0;

	error = mt7921_dma_tx(sc, &sc->sc_tx_wm, txd, total);
	if (error != 0 || !wait)
		goto out;

	/*
	 * Wait for the reply. Poll the rings ourselves (works without
	 * interrupts) and sleep in between; the interrupt thread may also
	 * deliver it and wake us.
	 */
	deadline = ticks + MCU_TIMEOUT_MS * hz / 1000;
	for (;;) {
		mt7921_mcu_poll_locked(sc);
		if (mcu->got_resp || ticks - deadline >= 0)
			break;
		msleep(mcu, &sc->sc_mtx, 0, "mt7921mcu", MAX(1, hz / 1000));
	}
	mcu->wait_seq = 0;

	if (!mcu->got_resp) {
		device_printf(sc->sc_dev, "MCU command 0x%x (seq %u) timed out\n",
			cmd, seq);
		error = ETIMEDOUT;
		goto out;
	}

	if (cmd == MCU_CMD_PATCH_SEM_CONTROL || cmd == MCU_CMD_PATCH_FINISH_REQ) {
		error = mcu->resp_len >= MCU_RXD_SIZE - 4 + 1
			? mcu->resp[MCU_RXD_SIZE - 4] : EINVAL;
	} else
		error = mcu->resp_len >= MCU_RXD_SIZE ? 0 : EINVAL;

out:
	mcu->busy = false;
	wakeup(&mcu->busy);
	MT7921_UNLOCK(sc);
	return error;
}


/* #pragma mark - firmware download */


/* Port of mt76_connac_mcu_init_download(). */
static int
mt7921_mcu_init_download(struct mt7921_mcu* mcu, uint32_t addr, uint32_t len,
	uint32_t mode)
{
	uint8_t req[12];
	uint8_t cmd;

	put_le32(req + 0, addr);
	put_le32(req + 4, len);
	put_le32(req + 8, mode);

	if (addr == MCU_PATCH_ADDRESS || addr == 0x900000)
		cmd = MCU_CMD_PATCH_START_REQ;
	else
		cmd = MCU_CMD_TARGET_ADDRESS_LEN_REQ;

	return mt7921_mcu_send(mcu, cmd, req, sizeof(req), true);
}


/* Port of __mt76_mcu_send_firmware(). */
static int
mt7921_mcu_send_firmware(struct mt7921_mcu* mcu, const uint8_t* data, int len)
{
	while (len > 0) {
		int cur = MIN(len, FW_SCATTER_MAX_LEN);
		int ret = mt7921_mcu_send(mcu, MCU_CMD_FW_SCATTER, data, cur,
			false);
		if (ret != 0)
			return ret;
		data += cur;
		len -= cur;
	}
	return 0;
}


static int
mt7921_mcu_patch_sem(struct mt7921_mcu* mcu, bool get)
{
	uint8_t req[4];

	put_le32(req, get ? PATCH_SEM_GET : PATCH_SEM_RELEASE);
	return mt7921_mcu_send(mcu, MCU_CMD_PATCH_SEM_CONTROL, req,
		sizeof(req), true);
}


/* Port of mt76_connac2_get_data_mode() (connac2 path). */
static uint32_t
mt7921_patch_data_mode(uint32_t info)
{
	uint32_t mode = DL_MODE_NEED_RSP;

	if (info == PATCH_SEC_NOT_SUPPORT)
		return mode;

	switch (info >> PATCH_SEC_ENC_TYPE_SHIFT) {
		case PATCH_SEC_ENC_TYPE_PLAIN:
			break;
		case PATCH_SEC_ENC_TYPE_AES:
			mode |= DL_MODE_ENCRYPT;
			mode |= ((info & PATCH_SEC_ENC_AES_KEY_MASK) << 1)
				& DL_MODE_KEY_IDX;
			mode |= DL_MODE_RESET_SEC_IV;
			break;
		case PATCH_SEC_ENC_TYPE_SCRAMBLE:
			mode |= DL_MODE_ENCRYPT;
			mode |= DL_CONFIG_ENCRY_MODE_SEL;
			mode |= DL_MODE_RESET_SEC_IV;
			break;
		default:
			break;
	}
	return mode;
}


/* Port of mt76_connac2_load_patch(). */
static int
mt7921_mcu_load_patch(struct mt7921_mcu* mcu)
{
	struct mt7921_softc* sc = mcu->sc;
	const struct firmware* fw = NULL;
	const uint8_t* data;
	size_t size;
	uint32_t n_region, i;
	int ret = 0, sem;

	sem = mt7921_mcu_patch_sem(mcu, true);
	switch (sem) {
		case PATCH_IS_DL:
			device_printf(sc->sc_dev, "ROM patch already loaded\n");
			return 0;
		case PATCH_NOT_DL_SEM_SUCCESS:
			break;
		default:
			device_printf(sc->sc_dev, "failed to get patch semaphore (%d)\n",
				sem);
			return EAGAIN;
	}

	fw = firmware_get(sc->sc_fw_patch);
	if (fw == NULL) {
		ret = ENOENT;
		goto out;
	}
	data = fw->data;
	size = fw->datasize;

	/* Port of mt76_connac2_patch_check(). */
	if (size < PATCH_HDR_SIZE) {
		ret = EINVAL;
		goto out;
	}
	n_region = get_be32(data + PATCH_HDR_N_REGION);
	if (n_region > (size - PATCH_HDR_SIZE) / PATCH_SEC_SIZE) {
		ret = EINVAL;
		goto out;
	}

	device_printf(sc->sc_dev, "ROM patch: HW/SW version 0x%x, built %.16s\n",
		get_be32(data + PATCH_HDR_HW_SW_VER),
		(const char*)data + PATCH_HDR_BUILD_DATE);

	for (i = 0; i < n_region; i++) {
		const uint8_t* sec = data + PATCH_HDR_SIZE + i * PATCH_SEC_SIZE;
		uint32_t offs = get_be32(sec + PATCH_SEC_OFFS);
		uint32_t addr = get_be32(sec + PATCH_SEC_ADDR);
		uint32_t len = get_be32(sec + PATCH_SEC_LEN);
		uint32_t mode;

		if ((get_be32(sec + PATCH_SEC_TYPE) & PATCH_SEC_TYPE_MASK)
				!= PATCH_SEC_TYPE_INFO
			|| offs > size || len > size - offs) {
			device_printf(sc->sc_dev, "invalid ROM patch section %u\n", i);
			ret = EINVAL;
			goto out;
		}

		mode = mt7921_patch_data_mode(get_be32(sec + PATCH_SEC_KEY_IDX));

		ret = mt7921_mcu_init_download(mcu, addr, len, mode);
		if (ret != 0) {
			device_printf(sc->sc_dev, "patch download request failed (%d)\n",
				ret);
			goto out;
		}
		ret = mt7921_mcu_send_firmware(mcu, data + offs, len);
		if (ret != 0) {
			device_printf(sc->sc_dev, "failed to send patch (%d)\n", ret);
			goto out;
		}
	}

	{
		uint8_t req[4] = { 0 };	/* check_crc = 0 */
		ret = mt7921_mcu_send(mcu, MCU_CMD_PATCH_FINISH_REQ, req,
			sizeof(req), true);
		if (ret != 0)
			device_printf(sc->sc_dev, "failed to start patch (%d)\n", ret);
	}

out:
	sem = mt7921_mcu_patch_sem(mcu, false);
	if (sem != PATCH_REL_SEM_SUCCESS) {
		device_printf(sc->sc_dev, "failed to release patch semaphore (%d)\n",
			sem);
		if (ret == 0)
			ret = EAGAIN;
	}
	if (fw != NULL)
		firmware_put(fw, FIRMWARE_UNLOAD);
	return ret;
}


/* Port of mt76_connac_mcu_gen_dl_mode() for connac2, WM firmware. */
static uint32_t
mt7921_ram_dl_mode(uint8_t feature_set)
{
	uint32_t mode = 0;

	if (feature_set & FW_FEATURE_SET_ENCRYPT)
		mode |= DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV;
	if (feature_set & FW_FEATURE_ENCRY_MODE)
		mode |= DL_CONFIG_ENCRY_MODE_SEL;
	mode |= feature_set & FW_FEATURE_SET_KEY_IDX;	/* same bit position */
	mode |= DL_MODE_NEED_RSP;
	return mode;
}


/* Port of mt76_connac2_load_ram() + mt76_connac_mcu_send_ram_firmware(). */
static int
mt7921_mcu_load_ram(struct mt7921_mcu* mcu)
{
	struct mt7921_softc* sc = mcu->sc;
	const struct firmware* fw;
	const uint8_t *data, *trailer;
	size_t size, avail, total = 0;
	uint32_t override = 0, option = 0, offset = 0;
	int n_region, i, ret = 0;

	fw = firmware_get(sc->sc_fw_ram);
	if (fw == NULL)
		return ENOENT;
	data = fw->data;
	size = fw->datasize;

	/* Port of mt76_connac2_fw_check(). */
	if (size < FW_TRAILER_SIZE) {
		device_printf(sc->sc_dev, "invalid RAM firmware: too small\n");
		ret = EINVAL;
		goto out;
	}
	trailer = data + size - FW_TRAILER_SIZE;
	n_region = trailer[FW_TRAILER_N_REGION];
	avail = size - FW_TRAILER_SIZE;
	if ((size_t)n_region > avail / FW_REGION_SIZE) {
		device_printf(sc->sc_dev, "invalid RAM firmware: %d regions\n",
			n_region);
		ret = EINVAL;
		goto out;
	}
	avail -= (size_t)n_region * FW_REGION_SIZE;
	for (i = 0; i < n_region; i++) {
		const uint8_t* region = trailer - (n_region - i) * FW_REGION_SIZE;
		uint32_t len = get_le32(region + FW_REGION_LEN);
		if (len > avail - total) {
			device_printf(sc->sc_dev, "invalid RAM firmware: region %d "
				"length 0x%x\n", i, len);
			ret = EINVAL;
			goto out;
		}
		total += len;
	}

	snprintf(sc->sc_fw_version, sizeof(sc->sc_fw_version), "%.10s-%.15s",
		(const char*)trailer + FW_TRAILER_FW_VER,
		(const char*)trailer + FW_TRAILER_BUILD_DATE);
	device_printf(sc->sc_dev, "WM firmware: version %s, %d regions\n",
		sc->sc_fw_version, n_region);

	for (i = 0; i < n_region; i++) {
		const uint8_t* region = trailer - (n_region - i) * FW_REGION_SIZE;
		uint8_t feature_set = region[FW_REGION_FEATURE_SET];
		uint32_t addr = get_le32(region + FW_REGION_ADDR);
		uint32_t len = get_le32(region + FW_REGION_LEN);

		if (feature_set & FW_FEATURE_NON_DL)
			goto next;
		if (feature_set & FW_FEATURE_OVERRIDE_ADDR)
			override = addr;

		ret = mt7921_mcu_init_download(mcu, addr, len,
			mt7921_ram_dl_mode(feature_set));
		if (ret != 0) {
			device_printf(sc->sc_dev, "RAM download request failed (%d)\n",
				ret);
			goto out;
		}
		ret = mt7921_mcu_send_firmware(mcu, data + offset, len);
		if (ret != 0) {
			device_printf(sc->sc_dev, "failed to send firmware (%d)\n", ret);
			goto out;
		}
next:
		offset += len;
	}

	if (override != 0)
		option |= FW_START_OVERRIDE;

	{
		uint8_t req[8];
		put_le32(req + 0, option);
		put_le32(req + 4, override);
		ret = mt7921_mcu_send(mcu, MCU_CMD_FW_START_REQ, req, sizeof(req),
			true);
		if (ret != 0)
			device_printf(sc->sc_dev, "failed to start WM firmware (%d)\n",
				ret);
	}

out:
	firmware_put(fw, FIRMWARE_UNLOAD);
	return ret;
}


/* #pragma mark - after boot */


/* Port of mt7921_mcu_get_nic_capability(), MAC address only. */
static int
mt7921_mcu_get_nic_capability(struct mt7921_mcu* mcu)
{
	struct mt7921_softc* sc = mcu->sc;
	const uint8_t* p;
	int ret, left, n, i;
	bool found = false;

	ret = mt7921_mcu_send(mcu, MCU_CE_CMD(MCU_CE_CMD_GET_NIC_CAPAB), NULL,
		0, true);
	if (ret != 0)
		return ret;

	p = mcu->resp + MCU_RXD_SIZE;
	left = mcu->resp_len - MCU_RXD_SIZE;
	if (left < 4)
		return EINVAL;

	n = p[0] | (p[1] << 8);
	p += 4;
	left -= 4;

	for (i = 0; i < n && left >= 8; i++) {
		uint32_t type = get_le32(p);
		uint32_t len = get_le32(p + 4);
		p += 8;
		left -= 8;
		if (len > (uint32_t)left)
			break;
		if (type == MT_NIC_CAP_MAC_ADDR && len >= 6) {
			memcpy(sc->sc_macaddr, p, 6);
			found = true;
		}
		p += len;
		left -= len;
	}

	device_printf(sc->sc_dev, "NIC capability: %d elements\n", n);
	return found ? 0 : ENOENT;
}


/* #pragma mark - entry point */


/* #pragma mark - lifecycle */


int
mt7921_mcu_alloc(struct mt7921_softc* sc)
{
	struct mt7921_mcu* mcu;

	mcu = malloc(sizeof(*mcu), M_DEVBUF, M_WAITOK | M_ZERO);
	if (mcu == NULL)
		return ENOMEM;
	mcu->sc = sc;
	mcu->txbuf = malloc(4096, M_DEVBUF, M_WAITOK | M_ZERO);
	mcu->resp = malloc(MCU_RESP_MAX, M_DEVBUF, M_WAITOK | M_ZERO);
	if (mcu->txbuf == NULL || mcu->resp == NULL) {
		free(mcu->txbuf, M_DEVBUF);
		free(mcu->resp, M_DEVBUF);
		free(mcu, M_DEVBUF);
		return ENOMEM;
	}
	sc->sc_mcu = mcu;
	return 0;
}


void
mt7921_mcu_free(struct mt7921_softc* sc)
{
	struct mt7921_mcu* mcu = sc->sc_mcu;

	if (mcu == NULL)
		return;
	sc->sc_mcu = NULL;
	free(mcu->txbuf, M_DEVBUF);
	free(mcu->resp, M_DEVBUF);
	free(mcu, M_DEVBUF);
}


/*
 * Port of mt7921e_mcu_init() + mt792x_load_firmware() + the first part of
 * mt7921_run_firmware(). Requires DMA to be up and sc->sc_mcu allocated.
 */
int
mt7921_mcu_run_firmware(struct mt7921_softc* sc)
{
	struct mt7921_mcu* mcu = sc->sc_mcu;
	uint32_t reg;
	int ret;

	if (mcu == NULL)
		return ENXIO;

	/* Force the firmware into normal mode before download (init.c). */
	mt7921_wr(sc, MT_SWDEF_MODE, MT_SWDEF_NORMAL_MODE);

	/* mt7921e_driver_own() */
	reg = mt7921_l1_map(sc, MT_TOP_LPCR_HOST_BAND0);
	mt7921_wr(sc, reg, MT_TOP_LPCR_HOST_DRV_OWN);
	if (!mt7921_poll_msec(sc, reg, MT_TOP_LPCR_HOST_FW_OWN, 0, 500, 1)) {
		device_printf(sc->sc_dev, "timeout for driver own\n");
		return EIO;
	}

	mt7921_set(sc, MT_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS);

	/* mt76_connac_mcu_restart() */
	{
		uint8_t req[4] = { 1, 0, 0, 0 };	/* power_mode = 1 */
		ret = mt7921_mcu_send(mcu, MCU_CMD_NIC_POWER_CTRL, req,
			sizeof(req), false);
		if (ret != 0)
			return ret;
	}

	if (!mt7921_poll_msec(sc, MT_CONN_ON_MISC, MT_TOP_MISC_FW_STATE,
			MT_TOP_MISC2_FW_PWR_ON, 1000, 1))
		device_printf(sc->sc_dev, "MCU not ready for firmware download "
			"(continuing)\n");

	ret = mt7921_mcu_load_patch(mcu);
	if (ret != 0)
		return ret;
	device_printf(sc->sc_dev, "ROM patch loaded\n");

	ret = mt7921_mcu_load_ram(mcu);
	if (ret != 0)
		return ret;

	if (!mt7921_poll_msec(sc, MT_CONN_ON_MISC, MT_TOP_MISC2_FW_N9_RDY,
			MT_TOP_MISC2_FW_N9_RDY, 1500, 1)) {
		device_printf(sc->sc_dev, "timeout waiting for firmware to start\n");
		return EIO;
	}
	device_printf(sc->sc_dev, "firmware running\n");

	ret = mt7921_mcu_get_nic_capability(mcu);
	if (ret != 0) {
		device_printf(sc->sc_dev, "could not read NIC capability (%d)\n",
			ret);
		return ret;
	}
	device_printf(sc->sc_dev, "MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
		sc->sc_macaddr[0], sc->sc_macaddr[1], sc->sc_macaddr[2],
		sc->sc_macaddr[3], sc->sc_macaddr[4], sc->sc_macaddr[5]);
	return 0;
}


/* #pragma mark - MAC / radio setup (milestone 5) */


/* Channels we tell the firmware about; same lists as mt7921_net.c. */
static const uint8_t mt7921_fw_chan_2ghz[] = MT7921_CHANS_2GHZ;
static const uint8_t mt7921_fw_chan_5ghz[] = MT7921_CHANS_5GHZ;

/* Linux ieee80211_channel_flags, which the firmware takes as is. */
#define MT7921_FW_CHAN_NO_IR	(1U << 1)
#define MT7921_FW_CHAN_RADAR	(1U << 3)


/* Port of mt792x_mac_init_band(). */
static void
mt7921_mac_init_band(struct mt7921_softc* sc, int band)
{
	mt7921_rmw(sc, MT_TMAC_CTCR0(band), MT_TMAC_CTCR0_INS_DDLMT_REFTIME,
		0x3f);
	mt7921_set(sc, MT_TMAC_CTCR0(band),
		MT_TMAC_CTCR0_INS_DDLMT_VHT_SMPDU_EN | MT_TMAC_CTCR0_INS_DDLMT_EN);

	mt7921_set(sc, MT_WF_RMAC_MIB_TIME0(band), MT_WF_RMAC_MIB_RXTIME_EN);
	mt7921_set(sc, MT_WF_RMAC_MIB_AIRTIME0(band), MT_WF_RMAC_MIB_RXTIME_EN);

	mt7921_set(sc, MT_MIB_SCR1(band), MT_MIB_TXDUR_EN | MT_MIB_RXDUR_EN);

	mt7921_rmw(sc, MT_DMA_DCR0(band), MT_DMA_DCR0_MAX_RX_LEN, 1536 << 3);
	/* disable rx rate report by default due to hw issues */
	mt7921_clear(sc, MT_DMA_DCR0(band), MT_DMA_DCR0_RXD_G5_EN);

	/* filter out non-resp frames and get instantaneous signal reporting */
	mt7921_rmw(sc, MT_WTBLOFF_TOP_RSCR(band),
		MT_WTBLOFF_TOP_RSCR_RCPI_MODE | MT_WTBLOFF_TOP_RSCR_RCPI_PARAM,
		(0U << 30) | (0x3U << 24));
}


/*
 * Everything mt7921_init_hardware(), mt7921_start() and
 * mt7921_add_interface() send before the first scan, for one station
 * interface (bss_idx 0, omac 0, band 0, WTBL index MT7921_WCID_BSS).
 *
 * Differences from upstream: no CLC/regulatory blob (country "00"), no
 * SAR/TX power table (firmware defaults), RX header translation and
 * A-MSDU de-aggregation left off so all frames arrive as plain 802.11.
 */
int
mt7921_mcu_init_mac(struct mt7921_softc* sc)
{
	struct mt7921_mcu* mcu = sc->sc_mcu;
	uint8_t buf[12 + 8 * 48];
	int ret, i, n;

	/* mt7921_mcu_set_eeprom(): use the on-chip efuse. */
	{
		uint8_t req[4] = { 0 /* EE_MODE_EFUSE */, 1 /* EE_FORMAT_WHOLE */,
			0, 0 };
		ret = mt7921_mcu_send(mcu, MCU_EXT_CMD(MCU_EXT_CMD_EFUSE_BUFFER_MODE),
			req, sizeof(req), true);
		if (ret != 0) {
			device_printf(sc->sc_dev, "EFUSE mode failed (%d)\n", ret);
			return ret;
		}
	}

	/* mt7921_mac_init() */
	mt7921_rmw(sc, MT_MDP_DCR1, MT_MDP_DCR1_MAX_RX_LEN, 1536 << 3);
	mt7921_mac_init_band(sc, 0);
	mt7921_mac_init_band(sc, 1);

	/* mt76_connac_mcu_set_mac_enable(band 0) */
	{
		uint8_t req[4] = { 1 /* enable */, 0 /* band */, 0, 0 };
		ret = mt7921_mcu_send(mcu, MCU_EXT_CMD(MCU_EXT_CMD_MAC_INIT_CTRL),
			req, sizeof(req), true);
		if (ret != 0) {
			device_printf(sc->sc_dev, "MAC enable failed (%d)\n", ret);
			return ret;
		}
	}

	/* mt76_connac_mcu_set_channel_domain() */
	memset(buf, 0, sizeof(buf));
	buf[0] = '0';
	buf[1] = '0';			/* alpha2 "00": world */
	buf[4] = 0;			/* bw_2g: 20/40 */
	buf[5] = 3;			/* bw_5g: 20/40/80/160 */
	buf[6] = 3;			/* bw_6g */
	n = 12;
	for (i = 0; i < (int)nitems(mt7921_fw_chan_2ghz); i++, n += 8)
		put_le16(buf + n, mt7921_fw_chan_2ghz[i]);	/* flags 0 */
	for (i = 0; i < (int)nitems(mt7921_fw_chan_5ghz); i++, n += 8) {
		put_le16(buf + n, mt7921_fw_chan_5ghz[i]);
		if (MT7921_CHAN_IS_DFS(mt7921_fw_chan_5ghz[i]))
			put_le32(buf + n + 4,
				MT7921_FW_CHAN_NO_IR | MT7921_FW_CHAN_RADAR);
	}
	buf[8] = nitems(mt7921_fw_chan_2ghz);
	buf[9] = nitems(mt7921_fw_chan_5ghz);
	buf[10] = 0;
	ret = mt7921_mcu_send(mcu, MCU_CE_CMD(MCU_CE_CMD_SET_CHAN_DOMAIN), buf, n,
		false);
	if (ret != 0)
		return ret;

	/* mt7921_mcu_set_chan_info(SET_RX_PATH): channel 1, 20 MHz, 2x2. */
	{
		uint8_t req[76];
		memset(req, 0, sizeof(req));
		req[0] = 1;			/* control_ch */
		req[1] = 1;			/* center_ch */
		req[2] = 0;			/* bw: CMD_CBW_20MHZ */
		req[3] = 2;			/* tx_streams_num */
		req[4] = 0x3;			/* rx_streams (mask) */
		req[5] = 0;			/* switch_reason: CH_SWITCH_NORMAL */
		req[6] = 0;			/* band_idx */
		req[10] = 0;			/* channel_band: 2.4 GHz */
		ret = mt7921_mcu_send(mcu, MCU_EXT_CMD(MCU_EXT_CMD_SET_RX_PATH),
			req, sizeof(req), true);
		if (ret != 0) {
			device_printf(sc->sc_dev, "set RX path failed (%d)\n", ret);
			return ret;
		}
	}

	/* mt76_connac_mcu_uni_add_dev(): DEV_INFO, then BSS_INFO basic. */
	{
		uint8_t dev_req[16];
		memset(dev_req, 0, sizeof(dev_req));
		dev_req[0] = 0;				/* omac_idx */
		dev_req[1] = 0;				/* band_idx */
		put_le16(dev_req + 4, 0);		/* tag: DEV_INFO_ACTIVE */
		put_le16(dev_req + 6, 12);		/* len */
		dev_req[8] = 1;				/* active */
		dev_req[9] = 0;				/* link_idx */
		memcpy(dev_req + 10, sc->sc_macaddr, 6);
		ret = mt7921_mcu_send(mcu, MCU_UNI_CMD(MCU_UNI_CMD_DEV_INFO_UPDATE),
			dev_req, sizeof(dev_req), true);
		if (ret != 0) {
			device_printf(sc->sc_dev, "DEV_INFO failed (%d)\n", ret);
			return ret;
		}
	}
	ret = mt7921_mcu_bss_basic(sc, NULL, true);
	if (ret != 0) {
		device_printf(sc->sc_dev, "BSS_INFO failed (%d)\n", ret);
		return ret;
	}

	device_printf(sc->sc_dev, "MAC and radio set up\n");
	return 0;
}


/*
 * BSS_INFO_UPDATE with the basic TLV (struct mt76_connac_bss_basic_tlv).
 * bssid NULL: not associated.
 */
int
mt7921_mcu_bss_basic(struct mt7921_softc* sc, const uint8_t* bssid,
	bool active)
{
	uint8_t req[4 + 32];

	memset(req, 0, sizeof(req));
	req[0] = 0;				/* bss_idx */
	put_le16(req + 4, 0);			/* tag: UNI_BSS_INFO_BASIC */
	put_le16(req + 6, 32);			/* len */
	req[8] = active;			/* active */
	req[9] = 0;				/* omac_idx */
	req[10] = 0;				/* hw_bss_idx: HW_BSSID_0 */
	req[11] = 0;				/* band_idx */
	put_le32(req + 12, 0x10001);		/* conn_type: CONNECTION_INFRA_STA */
	req[16] = 1;				/* conn_state */
	req[17] = 0;				/* wmm_idx */
	if (bssid != NULL)
		memcpy(req + 18, bssid, 6);
	put_le16(req + 24, MT7921_WCID_BSS);	/* bmc_tx_wlan_idx */
	put_le16(req + 30, MT7921_WCID_BSS);	/* sta_idx */
	return mt7921_mcu_send(sc->sc_mcu, MCU_UNI_CMD(MCU_UNI_CMD_BSS_INFO_UPDATE),
		req, sizeof(req), true);
}


/* #pragma mark - scanning */


/* struct mt76_connac_hw_scan_req */
struct mt7921_scan_ssid {
	uint32_t	ssid_len;
	uint8_t		ssid[32];
} __packed;

struct mt7921_scan_chan {
	uint8_t		band;		/* 1: 2.4 GHz, 2: 5 GHz */
	uint8_t		channel_num;
} __packed;

struct mt7921_hw_scan_req {
	uint8_t		seq_num;
	uint8_t		bss_idx;
	uint8_t		scan_type;
	uint8_t		ssid_type;
	uint8_t		ssids_num;
	uint8_t		probe_req_num;
	uint8_t		scan_func;
	uint8_t		version;
	struct mt7921_scan_ssid ssids[4];
	uint16_t	probe_delay_time;
	uint16_t	channel_dwell_time;
	uint16_t	timeout_value;
	uint8_t		channel_type;
	uint8_t		channels_num;
	struct mt7921_scan_chan channels[32];
	uint16_t	ies_len;
	uint8_t		ies[600];
	uint8_t		ext_channels_num;
	uint8_t		ext_ssids_num;
	uint16_t	channel_min_dwell_time;
	struct mt7921_scan_chan ext_channels[32];
	struct mt7921_scan_ssid ext_ssids[6];
	uint8_t		bssid[6];
	uint8_t		random_mac[6];
	uint8_t		pad[63];
	uint8_t		ssid_type_ext;
} __packed;

#define SCAN_FUNC_SPLIT_SCAN		BIT(5)


/*
 * Port of mt76_connac_mcu_hw_scan(). chans holds up to 64 (band, channel)
 * pairs; ssids are the non-empty SSIDs to probe for (active scan).
 */
int
mt7921_mcu_hw_scan(struct mt7921_softc* sc, bool active,
	const struct mt7921_scan_target* ssids, int n_ssids,
	const uint8_t (*chans)[2], int n_chans)
{
	struct mt7921_hw_scan_req* req;
	int i, ret;

	req = malloc(sizeof(*req), M_DEVBUF, M_WAITOK | M_ZERO);
	if (req == NULL)
		return ENOMEM;

	sc->sc_scan_seq = (sc->sc_scan_seq + 1) & 0x7f;
	req->seq_num = sc->sc_scan_seq;		/* | band_idx << 7 */
	req->bss_idx = 0;
	req->scan_type = active ? 1 : 0;
	req->probe_req_num = active ? 2 : 0;
	req->version = 1;

	n_ssids = MIN(n_ssids, 4);
	for (i = 0; i < n_ssids; i++) {
		int len = MIN(ssids[i].len, 32);
		req->ssids[i].ssid_len = htole32(len);
		memcpy(req->ssids[i].ssid, ssids[i].ssid, len);
	}
	req->ssid_type = n_ssids ? BIT(2) : BIT(0);
	req->ssid_type_ext = n_ssids ? BIT(0) : 0;
	req->ssids_num = n_ssids;

	/* connac2: duration 0 = firmware defaults */
	req->timeout_value = 0;
	req->channel_min_dwell_time = 0;
	req->channel_dwell_time = 0;

	if (n_chans <= 0 || n_chans > 64) {
		req->channel_type = 0;		/* all channels */
	} else {
		req->channel_type = 4;		/* specified channels */
		req->channels_num = MIN(n_chans, 32);
		req->ext_channels_num = MAX(n_chans - 32, 0);
		for (i = 0; i < n_chans; i++) {
			struct mt7921_scan_chan* c = i < 32
				? &req->channels[i] : &req->ext_channels[i - 32];
			c->band = chans[i][0];
			c->channel_num = chans[i][1];
		}
	}

	req->scan_func |= SCAN_FUNC_SPLIT_SCAN;
	memset(req->bssid, 0xff, 6);

	ret = mt7921_mcu_send(sc->sc_mcu, MCU_CE_CMD(MCU_CE_CMD_START_HW_SCAN),
		req, sizeof(*req), false);
	free(req, M_DEVBUF);
	return ret;
}


/* Port of mt76_connac_mcu_cancel_hw_scan() (without the completion). */
int
mt7921_mcu_cancel_hw_scan(struct mt7921_softc* sc)
{
	uint8_t req[4] = { sc->sc_scan_seq, 0, 0, 0 };

	return mt7921_mcu_send(sc->sc_mcu, MCU_CE_CMD(MCU_CE_CMD_CANCEL_HW_SCAN),
		req, sizeof(req), false);
}


/* #pragma mark - station / BSS records (milestone 6) */


/*
 * Message structures, copied from mt76_connac_mcu.h with only the type
 * names changed (__le16 -> uint16_t etc., all little endian on x86).
 */
struct mt7921_tlv {
	uint16_t	tag;
	uint16_t	len;
} __packed;

struct mt7921_sta_req_hdr {
	uint8_t		bss_idx;
	uint8_t		wlan_idx_lo;
	uint16_t	tlv_num;
	uint8_t		is_tlv_append;
	uint8_t		muar_idx;
	uint8_t		wlan_idx_hi;
	uint8_t		rsv;
} __packed;

struct mt7921_sta_rec_basic {
	uint16_t	tag;
	uint16_t	len;
	uint32_t	conn_type;
	uint8_t		conn_state;
	uint8_t		qos;
	uint16_t	aid;
	uint8_t		peer_addr[6];
	uint16_t	extra_info;
} __packed;

struct mt7921_sta_rec_phy {
	uint16_t	tag;
	uint16_t	len;
	uint16_t	basic_rate;
	uint8_t		phy_type;
	uint8_t		ampdu;
	uint8_t		rts_policy;
	uint8_t		rcpi;
	uint8_t		max_ampdu_len;
	uint8_t		rsv[1];
} __packed;

struct mt7921_sta_rec_ra_info {
	uint16_t	tag;
	uint16_t	len;
	uint16_t	legacy;
	uint8_t		rx_mcs_bitmask[10];
} __packed;

struct mt7921_sta_rec_state {
	uint16_t	tag;
	uint16_t	len;
	uint32_t	flags;
	uint8_t		state;
	uint8_t		vht_opmode;
	uint8_t		action;
	uint8_t		rsv[1];
} __packed;

struct mt7921_wtbl_req_hdr {
	uint8_t		wlan_idx_lo;
	uint8_t		operation;
	uint16_t	tlv_num;
	uint8_t		wlan_idx_hi;
	uint8_t		rsv[3];
} __packed;

struct mt7921_wtbl_generic {
	uint16_t	tag;
	uint16_t	len;
	uint8_t		peer_addr[6];
	uint8_t		muar_idx;
	uint8_t		skip_tx;
	uint8_t		cf_ack;
	uint8_t		qos;
	uint8_t		mesh;
	uint8_t		adm;
	uint16_t	partial_aid;
	uint8_t		baf_en;
	uint8_t		aad_om;
} __packed;

struct mt7921_wtbl_rx {
	uint16_t	tag;
	uint16_t	len;
	uint8_t		rcid;
	uint8_t		rca1;
	uint8_t		rca2;
	uint8_t		rv;
	uint8_t		rsv[4];
} __packed;

struct mt7921_wtbl_hdr_trans {
	uint16_t	tag;
	uint16_t	len;
	uint8_t		to_ds;
	uint8_t		from_ds;
	uint8_t		no_rx_trans;
	uint8_t		rsv;
} __packed;

/* Tags (mt76_connac_mcu.h enums). */
#define STA_REC_BASIC			0x00
#define STA_REC_RA			0x01
#define STA_REC_STATE			0x07
#define STA_REC_WTBL			0x0d
#define STA_REC_PHY			0x15
#define WTBL_GENERIC			0x00
#define WTBL_RX				0x01
#define WTBL_HDR_TRANS			0x06
#define WTBL_RESET_AND_SET		1
#define UNI_BSS_INFO_BASIC		0
#define UNI_BSS_INFO_RLM		2
#define UNI_BSS_INFO_QBSS		15
#define UNI_ROC_ACQUIRE			0
#define UNI_ROC_ABORT			1
#define MT7921_ROC_REQ_JOIN		0

#define CONNECTION_INFRA_STA		(BIT(0) | BIT(16))
#define CONNECTION_INFRA_AP		(BIT(1) | BIT(16))
#define CONN_STATE_DISCONNECT		0
#define CONN_STATE_PORT_SECURE		2
#define EXTRA_INFO_VER			BIT(0)
#define EXTRA_INFO_NEW			BIT(1)
/* mt76_connac_get_phy_mode() (BSS basic) and _v2() (phy type), legacy */
#define PHY_MODE_A			BIT(0)
#define PHY_MODE_B			BIT(1)
#define PHY_MODE_G			BIT(2)
#define PHY_TYPE_BIT_HR_DSSS		BIT(0)
#define PHY_TYPE_BIT_ERP		BIT(1)
#define PHY_TYPE_BIT_OFDM		BIT(3)


/* Minimal skb_put()-style builder for TLV messages. */
struct mt7921_msg {
	uint8_t		buf[512];
	int		len;
};


static void*
msg_put(struct mt7921_msg* m, int len)
{
	void* p;

	if (m->len + len > (int)sizeof(m->buf))
		return NULL;
	p = m->buf + m->len;
	memset(p, 0, len);
	m->len += len;
	return p;
}


/*
 * Port of mt76_connac_mcu_add_nested_tlv(): append a TLV, bump the TLV
 * count at ntlv_off (a sta_req_hdr or wtbl_req_hdr; tlv_num is at +2 in
 * both) and, if wtbl_off >= 0, grow the enclosing STA_REC_WTBL TLV.
 */
static void*
msg_add_tlv(struct mt7921_msg* m, int tag, int len, int ntlv_off, int wtbl_off)
{
	struct mt7921_tlv* t = msg_put(m, len);
	uint16_t* ntlv;

	if (t == NULL)
		return NULL;
	t->tag = htole16(tag);
	t->len = htole16(len);

	ntlv = (uint16_t*)(m->buf + ntlv_off + 2);
	*ntlv = htole16(le16toh(*ntlv) + 1);
	if (wtbl_off >= 0) {
		struct mt7921_tlv* w = (struct mt7921_tlv*)(m->buf + wtbl_off);
		w->len = htole16(le16toh(w->len) + len);
	}
	return t;
}


/*
 * Port of mt7921_mcu_sta_update() -> mt76_connac_mcu_sta_cmd() for the AP
 * of a legacy (non-HT) station interface, without hardware crypto.
 */
int
mt7921_mcu_sta_update(struct mt7921_softc* sc,
	const struct mt7921_sta_info* info)
{
	struct mt7921_msg* m;
	struct mt7921_sta_req_hdr* hdr;
	struct mt7921_sta_rec_basic* basic;
	struct mt7921_wtbl_req_hdr* whdr;
	int wtbl_off, whdr_off, conn_state, ret;
	const int wcid = MT7921_WCID_STA;

	m = malloc(sizeof(*m), M_DEVBUF, M_WAITOK | M_ZERO);
	if (m == NULL)
		return ENOMEM;

	/* mt76_connac_mcu_alloc_sta_req() */
	hdr = msg_put(m, sizeof(*hdr));
	hdr->bss_idx = 0;
	hdr->muar_idx = 0;			/* omac_idx (wcid is a station) */
	hdr->is_tlv_append = 1;
	hdr->wlan_idx_lo = wcid & 0xff;
	hdr->wlan_idx_hi = (wcid >> 8) & 0x7;

	/* mt76_connac_mcu_sta_basic_tlv() */
	conn_state = info->enable ? CONN_STATE_PORT_SECURE : CONN_STATE_DISCONNECT;
	basic = msg_add_tlv(m, STA_REC_BASIC, sizeof(*basic), 0, -1);
	basic->extra_info = htole16(EXTRA_INFO_VER
		| (info->newly && conn_state != CONN_STATE_DISCONNECT
			? EXTRA_INFO_NEW : 0));
	basic->conn_state = conn_state;
	basic->conn_type = htole32(CONNECTION_INFRA_AP);
	basic->aid = htole16(info->aid);
	memcpy(basic->peer_addr, info->addr, 6);
	basic->qos = 0;

	if (info->enable) {
		/* mt76_connac_mcu_sta_tlv(), legacy rates only */
		struct mt7921_sta_rec_phy* phy;
		struct mt7921_sta_rec_ra_info* ra;
		struct mt7921_sta_rec_state* st;
		uint16_t legacy;

		phy = msg_add_tlv(m, STA_REC_PHY, sizeof(*phy), 0, -1);
		phy->phy_type = info->is_5ghz ? PHY_TYPE_BIT_OFDM
			: (PHY_TYPE_BIT_HR_DSSS | PHY_TYPE_BIT_ERP);
		phy->basic_rate = htole16(info->basic_rates);
		phy->rcpi = 220;		/* to_rcpi(0), as before averaging */

		ra = msg_add_tlv(m, STA_REC_RA, sizeof(*ra), 0, -1);
		if (info->is_5ghz)
			legacy = (info->supp_rates & 0xff) << 6;
		else
			legacy = (((info->supp_rates >> 4) & 0xff) << 6)
				| (info->supp_rates & 0xf);
		ra->legacy = htole16(legacy);

		st = msg_add_tlv(m, STA_REC_STATE, sizeof(*st), 0, -1);
		st->state = info->state;
	}

	/* STA_REC_WTBL + mt76_connac_mcu_alloc_wtbl_req(WTBL_RESET_AND_SET) */
	wtbl_off = m->len;
	msg_add_tlv(m, STA_REC_WTBL, sizeof(struct mt7921_tlv), 0, -1);
	whdr_off = m->len;
	whdr = msg_put(m, sizeof(*whdr));
	whdr->operation = WTBL_RESET_AND_SET;
	whdr->wlan_idx_lo = wcid & 0xff;
	whdr->wlan_idx_hi = (wcid >> 8) & 0x7;
	{
		struct mt7921_tlv* w = (struct mt7921_tlv*)(m->buf + wtbl_off);
		w->len = htole16(le16toh(w->len) + sizeof(*whdr));
	}

	if (info->enable) {
		struct mt7921_wtbl_generic* gen;
		struct mt7921_wtbl_rx* rx;
		struct mt7921_wtbl_hdr_trans* htr;

		/* mt76_connac_mcu_wtbl_generic_tlv() */
		gen = msg_add_tlv(m, WTBL_GENERIC, sizeof(*gen), whdr_off, wtbl_off);
		gen->partial_aid = htole16(info->aid);
		memcpy(gen->peer_addr, info->addr, 6);
		gen->muar_idx = 0;
		gen->qos = 0;

		rx = msg_add_tlv(m, WTBL_RX, sizeof(*rx), whdr_off, wtbl_off);
		rx->rca1 = 1;
		rx->rca2 = 1;
		rx->rv = 1;

		/* mt76_connac_mcu_wtbl_hdr_trans_tlv(): station, no RX translation */
		htr = msg_add_tlv(m, WTBL_HDR_TRANS, sizeof(*htr), whdr_off,
			wtbl_off);
		htr->to_ds = 1;
		htr->no_rx_trans = 1;
	}

	ret = mt7921_mcu_send(sc->sc_mcu, MCU_UNI_CMD(MCU_UNI_CMD_STA_REC_UPDATE),
		m->buf, m->len, true);
	free(m, M_DEVBUF);
	return ret;
}


/*
 * Port of mt76_connac_mcu_uni_add_bss() + mt76_connac_mcu_uni_set_chctx()
 * for a legacy 20 MHz station BSS.
 */
int
mt7921_mcu_bss_update(struct mt7921_softc* sc,
	const struct mt7921_bss_info* bi)
{
	uint8_t req[4 + 32 + 8];
	uint8_t rlm[4 + 16];
	int ret;

	memset(req, 0, sizeof(req));
	req[0] = 0;					/* bss_idx */
	/* struct mt76_connac_bss_basic_tlv */
	put_le16(req + 4, UNI_BSS_INFO_BASIC);
	put_le16(req + 6, 32);
	req[8] = 1;					/* active (keep) */
	req[9] = 0;					/* omac_idx */
	req[10] = 0;					/* hw_bss_idx */
	req[11] = 0;					/* band_idx */
	put_le32(req + 12, CONNECTION_INFRA_STA);
	req[16] = !bi->enable;				/* conn_state */
	req[17] = 0;					/* wmm_idx */
	memcpy(req + 18, bi->bssid, 6);
	put_le16(req + 24, MT7921_WCID_BSS);		/* bmc_tx_wlan_idx */
	put_le16(req + 26, bi->beacon_int);
	req[28] = bi->dtim_period;
	req[29] = bi->is_5ghz ? PHY_MODE_A : (PHY_MODE_B | PHY_MODE_G);
	put_le16(req + 30, MT7921_WCID_BSS);		/* sta_idx */
	put_le16(req + 32, bi->is_5ghz ? PHY_TYPE_BIT_OFDM
		: (PHY_TYPE_BIT_HR_DSSS | PHY_TYPE_BIT_ERP));	/* nonht_basic_phy */
	/* struct mt76_connac_bss_qos_tlv */
	put_le16(req + 36, UNI_BSS_INFO_QBSS);
	put_le16(req + 38, 8);
	req[40] = 0;					/* qos */

	ret = mt7921_mcu_send(sc->sc_mcu, MCU_UNI_CMD(MCU_UNI_CMD_BSS_INFO_UPDATE),
		req, sizeof(req), true);
	if (ret != 0)
		return ret;

	/* RLM TLV: 20 MHz on the AP's channel. */
	memset(rlm, 0, sizeof(rlm));
	put_le16(rlm + 4, UNI_BSS_INFO_RLM);
	put_le16(rlm + 6, 16);
	rlm[8] = bi->channel;				/* control_channel */
	rlm[9] = bi->channel;				/* center_chan */
	rlm[10] = 0;					/* center_chan2 */
	rlm[11] = 0;					/* bw: CMD_CBW_20MHZ */
	rlm[12] = 2;					/* tx_streams */
	rlm[13] = 0x3;					/* rx_streams (chainmask) */
	rlm[14] = 1;					/* short_st */
	rlm[15] = 0;					/* ht_op_info */
	rlm[16] = 0;					/* sco */
	rlm[17] = bi->is_5ghz ? 1 : 0;			/* band: NL80211_BAND_* */
	return mt7921_mcu_send(sc->sc_mcu, MCU_UNI_CMD(MCU_UNI_CMD_BSS_INFO_UPDATE),
		rlm, sizeof(rlm), true);
}


/* Port of mt7921_mcu_set_roc() (join request, 20 MHz). */
int
mt7921_mcu_set_roc(struct mt7921_softc* sc, uint8_t channel, bool is_5ghz,
	uint32_t duration_ms)
{
	uint8_t req[4 + 24];

	sc->sc_roc_token++;
	memset(req, 0, sizeof(req));
	/* struct roc_acquire_tlv */
	put_le16(req + 4, UNI_ROC_ACQUIRE);
	put_le16(req + 6, 24);
	req[8] = 0;				/* bss_idx */
	req[9] = sc->sc_roc_token;		/* tokenid */
	req[10] = channel;			/* control_channel */
	req[11] = 0;				/* sco */
	req[12] = is_5ghz ? 2 : 1;		/* band */
	req[13] = 0;				/* bw: 20 MHz */
	req[14] = channel;			/* center_chan */
	req[15] = 0;				/* center_chan2 */
	req[16] = 0;				/* bw_from_ap */
	req[17] = channel;			/* center_chan_from_ap */
	req[18] = 0;				/* center_chan2_from_ap */
	req[19] = MT7921_ROC_REQ_JOIN;		/* reqtype */
	put_le32(req + 20, duration_ms);	/* maxinterval */
	req[24] = 0xff;				/* dbdcband: auto */
	return mt7921_mcu_send(sc->sc_mcu, MCU_UNI_CMD(MCU_UNI_CMD_ROC), req,
		sizeof(req), false);
}


/* Port of mt7921_mcu_abort_roc(). */
int
mt7921_mcu_abort_roc(struct mt7921_softc* sc)
{
	uint8_t req[4 + 12];

	memset(req, 0, sizeof(req));
	/* struct roc_abort_tlv */
	put_le16(req + 4, UNI_ROC_ABORT);
	put_le16(req + 6, 12);
	req[8] = 0;				/* bss_idx */
	req[9] = sc->sc_roc_token;		/* tokenid */
	req[10] = 0xff;				/* dbdcband: auto */
	return mt7921_mcu_send(sc->sc_mcu, MCU_UNI_CMD(MCU_UNI_CMD_ROC), req,
		sizeof(req), false);
}
