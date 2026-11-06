/*
 * Copyright 2026, Techniix. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Portions derived from MediaTek's mt76 driver (mt7921/mac.c,
 * mt7921/main.c):
 *   Copyright (C) 2020-2023 MediaTek Inc.
 *   SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * net80211 glue for MT7921/MT7922.
 *
 * Milestone 4: registration with net80211, VAP handling, safe stubs.
 * Milestone 5: receive path (RX descriptor parsing, delivery to net80211)
 * and firmware-offloaded scanning (IEEE80211_FEXT_SCAN_OFFLOAD): the
 * firmware scans all channels in one go, net80211 accepts beacons from any
 * channel while scanning, and the firmware's SCAN_DONE event ends the scan.
 * Milestone 6: transmission (connac TXD + TXP, tokens released by TX-free
 * events) and joining a network: station record + channel reservation
 * before authentication, BSS/channel/station updates on association.
 * Encryption (WPA2) is done in software by net80211.
 *
 * Structure follows FreeBSD's iwn(4) (if_iwn.c) as shipped in Haiku.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/taskqueue.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

#include <dev/mt7921/if_mt7921var.h>
#include <dev/mt7921/mt7921_reg.h>


/*
 * 2.4 GHz channels 1-11 and the 5 GHz UNII-1/2/2e/3 channels. The DFS
 * ones (UNII-2/2e) are marked passive. Proper regulatory handling (CLC /
 * country code from firmware) comes later.
 */
static const uint8_t mt7921_chan_2ghz[] = MT7921_CHANS_2GHZ;
static const uint8_t mt7921_chan_5ghz[] = MT7921_CHANS_5GHZ;

#define MT7921_SCAN_TIMEOUT_MS	10000
#define MT7921_TX_DATA_MAX	(2048 - 64)	/* slot minus TXD + TXP */

/* Haiku's printf has no %6D. */
#define MACFMT			"%02x:%02x:%02x:%02x:%02x:%02x"
#define MACARG(a)		(a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MT7921_NOISE_FLOOR	(-95)


static inline uint32_t
rd_le32(const uint8_t* p)
{
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[1] << 8) | p[0];
}


static void
mt7921_getradiocaps(struct ieee80211com* ic, int maxchans, int* nchans,
	struct ieee80211_channel chans[])
{
	struct mt7921_softc* sc = ic->ic_softc;
	uint8_t bands[IEEE80211_MODE_BYTES];
	int i;

	/* HT20 only for now (cbw_flags 0): no 40 MHz channels yet. */
	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	if (sc->sc_ht)
		setbit(bands, IEEE80211_MODE_11NG);
	ieee80211_add_channel_list_2ghz(chans, maxchans, nchans,
		mt7921_chan_2ghz, nitems(mt7921_chan_2ghz), bands, 0);

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11A);
	if (sc->sc_ht)
		setbit(bands, IEEE80211_MODE_11NA);
	ieee80211_add_channel_list_5ghz(chans, maxchans, nchans,
		mt7921_chan_5ghz, nitems(mt7921_chan_5ghz), bands, 0);

	/* No transmitting on radar channels until an AP has been heard. */
	for (i = 0; i < *nchans; i++) {
		if (IEEE80211_IS_CHAN_5GHZ(&chans[i])
			&& MT7921_CHAN_IS_DFS(chans[i].ic_ieee))
			chans[i].ic_flags |= IEEE80211_CHAN_PASSIVE | IEEE80211_CHAN_DFS;
	}
}


static void	mt7921_wme_apply(struct mt7921_softc*);


/* #pragma mark - receive */


static int
mt7921_rcpi_to_dbm(uint32_t rcpi)
{
	if (rcpi == 0 || rcpi == 0xff)
		return -127;
	return ((int)rcpi - 220) / 2;
}


/*
 * Parse one RX buffer (RXD + 802.11 frame) and queue the frame for
 * net80211. Port of the parts of mt7921_mac_fill_rx() that apply while RX
 * header translation and A-MSDU de-aggregation are off. Called with sc_mtx
 * held (from the RX ring dispatch); delivery happens in mt7921_rx_task().
 */
void
mt7921_rx_frame(struct mt7921_softc* sc, const uint8_t* data, int len)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ieee80211_rx_stats rxs;
	uint32_t rxd1, rxd2, rxd3, rxd4, v1 = 0;
	int off, hdr_gap, flen, chan, rssi, r0, r1;
	bool have_rxv = false;
	struct mbuf* m;

	if (!sc->sc_ic_attached || len < 24)
		goto drop;

	rxd1 = rd_le32(data + 4);
	rxd2 = rd_le32(data + 8);
	rxd3 = rd_le32(data + 12);
	rxd4 = rd_le32(data + 16);

	if ((rxd1 & (MT_RXD1_NORMAL_BAND_IDX | MT_RXD1_NORMAL_FCS_ERR
			| MT_RXD1_NORMAL_ICV_ERR | MT_RXD1_NORMAL_TKIP_MIC_ERR)) != 0
		|| (rxd2 & (MT_RXD2_NORMAL_AMSDU_ERR
			| MT_RXD2_NORMAL_MAX_LEN_ERROR)) != 0)
		goto drop;

	/* Header translation and de-aggregation are off; refuse otherwise. */
	if ((rxd2 & MT_RXD2_NORMAL_HDR_TRANS) != 0
		|| (rxd4 & MT_RXD4_NORMAL_PAYLOAD_FORMAT) != 0)
		goto drop;

	off = 6 * 4;
	if (rxd1 & MT_RXD1_NORMAL_GROUP_4)
		off += 4 * 4;
	if (rxd1 & MT_RXD1_NORMAL_GROUP_1)
		off += 4 * 4;
	if (rxd1 & MT_RXD1_NORMAL_GROUP_2)
		off += 2 * 4;
	if (rxd1 & MT_RXD1_NORMAL_GROUP_3) {
		if (off + 8 > len)
			goto drop;
		v1 = rd_le32(data + off + 4);	/* P-RXV word 1: RCPI */
		have_rxv = true;
		off += 2 * 4;
		if (rxd1 & MT_RXD1_NORMAL_GROUP_5) {
			off += 6 * 4;
			if (off + 4 > len)
				goto drop;
			v1 = rd_le32(data + off);
			off += 12 * 4;
		}
	}
	if (off >= len)
		goto drop;

	hdr_gap = off + 2 * ((rxd2 >> MT_RXD2_NORMAL_HDR_OFFSET_SHIFT) & 0x3);
	flen = len - hdr_gap;
	if (flen < (int)sizeof(struct ieee80211_frame_min) || flen > MJUMPAGESIZE)
		goto drop;

	chan = (rxd3 >> MT_RXD3_NORMAL_CH_FREQ_SHIFT) & 0xff;
	if (chan == 0 || chan > 180)	/* no 6 GHz yet */
		goto drop;

	rssi = -80;
	if (have_rxv) {
		r0 = mt7921_rcpi_to_dbm(v1 & 0xff);
		r1 = mt7921_rcpi_to_dbm((v1 >> 8) & 0xff);
		rssi = MAX(r0, r1);
		if (rssi < -100)
			rssi = -100;
	}

	if (flen <= MCLBYTES)
		m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
	else	/* an A-MSDU; net80211 splits it */
		m = m_getjcl(M_NOWAIT, MT_DATA, M_PKTHDR, MJUMPAGESIZE);
	if (m == NULL)
		goto drop;
	memcpy(mtod(m, uint8_t*), data + hdr_gap, flen);
	m->m_len = m->m_pkthdr.len = flen;

	memset(&rxs, 0, sizeof(rxs));
	rxs.r_flags = IEEE80211_R_NF | IEEE80211_R_RSSI | IEEE80211_R_IEEE
		| IEEE80211_R_FREQ | IEEE80211_R_BAND;
	rxs.c_ieee = chan;
	rxs.c_band = chan > 14 ? IEEE80211_CHAN_5GHZ : IEEE80211_CHAN_2GHZ;
	rxs.c_freq = ieee80211_ieee2mhz(chan, rxs.c_band);
	rxs.c_nf = MT7921_NOISE_FLOOR;
	rxs.c_rssi = MIN(MAX(rssi - MT7921_NOISE_FLOOR, 0), 127) * 2;
	ieee80211_add_rx_params(m, &rxs);

	if (sc->sc_rx_logged < 8) {
		const uint8_t* wh = data + hdr_gap;
		sc->sc_rx_logged++;
		device_printf(sc->sc_dev, "rx frame: fc %02x%02x len %d ch %d "
			"rssi %d dBm\n", wh[0], wh[1], flen, chan, rssi);
	}

	if (mbufq_enqueue(&sc->sc_rxq, m) != 0) {
		m_freem(m);
		goto drop;
	}
	sc->sc_rx_frames++;
	taskqueue_enqueue(ic->ic_tq, &sc->sc_rx_task);
	return;

drop:
	sc->sc_rx_dropped++;
}


/* Hand queued frames to net80211, without holding sc_mtx. */
static void
mt7921_rx_task(void* arg, int pending)
{
	struct mt7921_softc* sc = arg;
	struct ieee80211com* ic = &sc->sc_ic;
	struct ieee80211_node* ni;
	struct mbuf* m;

	for (;;) {
		MT7921_LOCK(sc);
		m = mbufq_dequeue(&sc->sc_rxq);
		MT7921_UNLOCK(sc);
		if (m == NULL)
			break;

		ni = ieee80211_find_rxnode(ic,
			mtod(m, struct ieee80211_frame_min*));
		if (ni != NULL) {
			(void)ieee80211_input_mimo(ni, m);
			ieee80211_free_node(ni);
		} else
			(void)ieee80211_input_mimo_all(ic, m);
	}
}


/* #pragma mark - scanning */


/* SCAN_DONE event from the firmware. Called with sc_mtx held. */
void
mt7921_scan_done_event(struct mt7921_softc* sc)
{
	if (!sc->sc_scanning)
		return;
	sc->sc_scanning = false;
	callout_stop(&sc->sc_scan_to);
	device_printf(sc->sc_dev, "scan done (%u frames received so far)\n",
		sc->sc_rx_frames);
	if (sc->sc_ic_attached)
		taskqueue_enqueue(sc->sc_ic.ic_tq, &sc->sc_scan_done_task);
}


/* Safety net if the firmware never reports. Runs with sc_mtx held. */
static void
mt7921_scan_timeout(void* arg)
{
	struct mt7921_softc* sc = arg;

	if (!sc->sc_scanning)
		return;
	sc->sc_scanning = false;
	device_printf(sc->sc_dev, "scan timed out\n");
	taskqueue_enqueue(sc->sc_ic.ic_tq, &sc->sc_scan_done_task);
}


static void
mt7921_scan_done_task(void* arg, int pending)
{
	struct mt7921_softc* sc = arg;
	struct ieee80211vap* vap = TAILQ_FIRST(&sc->sc_ic.ic_vaps);

	if (vap != NULL)
		ieee80211_scan_done(vap);
}


/*
 * Start of a net80211 scan: hand the whole channel list to the firmware.
 * Called from the net80211 taskqueue without locks held.
 */
static void
mt7921_scan_start(struct ieee80211com* ic)
{
	struct mt7921_softc* sc = ic->ic_softc;
	struct ieee80211_scan_state* ss = ic->ic_scan;
	struct mt7921_scan_target ssids[4];
	uint8_t chans[64][2];
	int i, j, n_chans = 0, n_ssids = 0, error;
	bool active;

	if (!sc->sc_radio_ready) {
		/* Stage 4: nothing will ever answer; end the scan right away. */
		taskqueue_enqueue(ic->ic_tq, &sc->sc_scan_done_task);
		return;
	}

	for (i = 0; i < ss->ss_last && n_chans < 64; i++) {
		struct ieee80211_channel* c = ss->ss_chans[i];
		uint8_t band = IEEE80211_IS_CHAN_2GHZ(c) ? 1 : 2;
		bool dup = false;

		/* net80211 lists 11b and 11g variants of the same channel. */
		for (j = 0; j < n_chans; j++) {
			if (chans[j][0] == band && chans[j][1] == c->ic_ieee) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			chans[n_chans][0] = band;
			chans[n_chans][1] = c->ic_ieee;
			n_chans++;
		}
	}

	for (i = 0; i < ss->ss_nssid && n_ssids < 4; i++) {
		if (ss->ss_ssid[i].len == 0)
			continue;
		ssids[n_ssids].len = MIN(ss->ss_ssid[i].len, 32);
		memcpy(ssids[n_ssids].ssid, ss->ss_ssid[i].ssid, ssids[n_ssids].len);
		n_ssids++;
	}
	active = (ss->ss_flags & IEEE80211_SCAN_ACTIVE) != 0;

	MT7921_LOCK(sc);
	if (sc->sc_scanning || sc->sc_dma_paused) {
		MT7921_UNLOCK(sc);
		taskqueue_enqueue(ic->ic_tq, &sc->sc_scan_done_task);
		return;
	}
	sc->sc_scanning = true;
	callout_reset(&sc->sc_scan_to, MT7921_SCAN_TIMEOUT_MS * hz / 1000,
		mt7921_scan_timeout, sc);
	MT7921_UNLOCK(sc);

	error = mt7921_mcu_hw_scan(sc, active, ssids, n_ssids, chans, n_chans);
	if (error != 0) {
		device_printf(sc->sc_dev, "starting scan failed (%d)\n", error);
		MT7921_LOCK(sc);
		sc->sc_scanning = false;
		callout_stop(&sc->sc_scan_to);
		MT7921_UNLOCK(sc);
		taskqueue_enqueue(ic->ic_tq, &sc->sc_scan_done_task);
		return;
	}

	device_printf(sc->sc_dev, "scan started: %d channels, %s, %d SSIDs\n",
		n_chans, active ? "active" : "passive", n_ssids);
}


/* End (or cancellation) of a net80211 scan. */
static void
mt7921_scan_end(struct ieee80211com* ic)
{
	struct mt7921_softc* sc = ic->ic_softc;
	bool cancel;

	MT7921_LOCK(sc);
	cancel = sc->sc_scanning;
	sc->sc_scanning = false;
	callout_stop(&sc->sc_scan_to);
	MT7921_UNLOCK(sc);

	if (cancel) {
		device_printf(sc->sc_dev, "cancelling firmware scan\n");
		(void)mt7921_mcu_cancel_hw_scan(sc);
	}
}


/* The firmware dwells on its own; net80211 must not arm channel timers. */
static void
mt7921_scan_curchan(struct ieee80211_scan_state* ss, unsigned long maxdwell)
{
}


static void
mt7921_scan_mindwell(struct ieee80211_scan_state* ss)
{
}


static void
mt7921_set_channel(struct ieee80211com* ic)
{
	/* The firmware switches channels itself (scan, ROC, BSS RLM). */
}


/* #pragma mark - transmit */


/* Index of a rate (500 kb/s units) in the band's rate table, or -1. */
static int
mt7921_rate_index(int rate, bool is_5ghz)
{
	static const uint8_t r2g[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
	static const uint8_t r5g[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
	const uint8_t* t = is_5ghz ? r5g : r2g;
	int i, n = is_5ghz ? (int)nitems(r5g) : (int)nitems(r2g);

	for (i = 0; i < n; i++) {
		if (t[i] == rate)
			return i;
	}
	return -1;
}


/* TX-free event (PKT_TYPE_TXRX_NOTIFY). Port of mt7921_mac_tx_free(). */
void
mt7921_tx_free_event(struct mt7921_softc* sc, const uint8_t* data, int len)
{
	int count, i;

	if (len < 8)
		return;
	count = (data[2] | (data[3] << 8)) & 0x3ff;	/* ctrl: MSDU_CNT */

	for (i = 0; i < count && 8 + 4 * (i + 1) <= len; i++) {
		uint32_t info = rd_le32(data + 8 + 4 * i);
		int token, stat;

		if (info & BIT(31)) {		/* MT_TX_FREE_PAIR: new wcid */
			count++;
			continue;
		}
		token = (info >> 16) & 0x7fff;	/* MT_TX_FREE_MSDU_ID */
		stat = (info >> 13) & 0x3;	/* MT_TX_FREE_STATUS */
		if (stat != 0)
			sc->sc_tx_failed++;
		if (sc->sc_txfree_logged < 8) {
			sc->sc_txfree_logged++;
			device_printf(sc->sc_dev, "tx done: token %d status %d\n",
				token, stat);
		}
		mt7921_dma_tx_token_free(sc, &sc->sc_tx_data0, token);
	}
}


/*
 * Queue one 802.11 frame. Port of mt7921e_tx_prepare_skb() +
 * mt76_connac2_mac_write_txwi() (802.11 header format) +
 * mt76_connac_write_hw_txp(). The frame is copied into the slot, so the
 * caller may complete the mbuf right away.
 */
static int
mt7921_tx(struct mt7921_softc* sc, struct ieee80211_node* ni, struct mbuf* m)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct ieee80211_channel* chan;
	struct ieee80211_frame* wh;
	uint8_t type, subtype, fc_type, fc_stype, tid, *buf;
	uint32_t w[8];
	bus_addr_t paddr;
	int len, hdrlen, idx, wcid, q_idx, i;
	bool mcast, fixed, is_5ghz;

	if (!sc->sc_can_tx)
		return ENETDOWN;

	wh = mtod(m, struct ieee80211_frame*);
	if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) {
		/* Software encryption (no keys in the hardware). */
		if (ieee80211_crypto_encap(ni, m) == NULL)
			return EIO;
		wh = mtod(m, struct ieee80211_frame*);
	}

	len = m->m_pkthdr.len;
	if (len < (int)sizeof(struct ieee80211_frame_min)
		|| len > MT7921_TX_DATA_MAX)
		return EMSGSIZE;

	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
	fc_type = type >> 2;
	fc_stype = subtype >> 4;
	mcast = IEEE80211_IS_MULTICAST(wh->i_addr1);
	hdrlen = ieee80211_anyhdrsize(wh);
	fixed = type != IEEE80211_FC0_TYPE_DATA || mcast;

	chan = (ni != NULL && ni->ni_chan != IEEE80211_CHAN_ANYC)
		? ni->ni_chan : ic->ic_curchan;
	is_5ghz = chan != NULL && IEEE80211_IS_CHAN_5GHZ(chan);

	wcid = sc->sc_sta_added && IEEE80211_ADDR_EQ(wh->i_addr1, sc->sc_bssid)
		? MT7921_WCID_STA : MT7921_WCID_BSS;
	/*
	 * Management/control: ALTX0. Data: the LMAC queue of its access
	 * category (mt76_connac_lmac_mapping(): BK 0, BE 1, VI 2, VO 3).
	 */
	tid = 0;
	if (type != IEEE80211_FC0_TYPE_DATA)
		q_idx = 0x10;
	else if (IEEE80211_QOS_HAS_SEQ(wh)) {
		static const uint8_t lmac_q[WME_NUM_AC] = {
			[WME_AC_BE] = 1, [WME_AC_BK] = 0,
			[WME_AC_VI] = 2, [WME_AC_VO] = 3
		};
		tid = ieee80211_gettid(wh) & 0x7;
		q_idx = lmac_q[TID_TO_WME_AC(tid)];
	} else
		q_idx = 1;

	memset(w, 0, sizeof(w));
	w[0] = ((uint32_t)(len + 32) & 0xffff)		/* TX_BYTES */
		| (0U << 23)				/* PKT_FMT: CT */
		| ((uint32_t)q_idx << 25);		/* Q_IDX */
	w[1] = BIT(31)					/* LONG_FORMAT */
		| ((uint32_t)wcid & 0x3ff)		/* WLAN_IDX */
		| (2U << 16)				/* HDR_FORMAT: 802.11 */
		| ((uint32_t)(hdrlen / 2) << 11)	/* HDR_INFO */
		| ((uint32_t)tid << 20);		/* TID */
	w[2] = ((uint32_t)fc_type << 4) | fc_stype
		| (mcast ? BIT(10) : 0);
	w[3] = 15U << 11;				/* REM_TX_COUNT */
	if (mcast)
		w[3] |= BIT(0);				/* NO_ACK */
	w[5] = 1;					/* PID: MT_PACKET_ID_NO_SKB */
	w[7] = ((uint32_t)fc_type << 20) | ((uint32_t)fc_stype << 16);
	if (fixed) {
		/* Lowest basic rate: CCK 1M (2.4 GHz) or OFDM 6M (5 GHz). */
		uint32_t rate = is_5ghz ? ((1U << 6) | 11) : 0;
		w[2] |= BIT(31) | BIT(13);		/* FIX_RATE, HTC_VLD */
		w[3] |= BIT(28);			/* BA_DISABLE */
		w[6] = BIT(2) | (rate << 16);		/* FIXED_BW, TX_RATE */
		w[7] |= 24U << 11;			/* SPE_IDX for 2 paths */
	}

	MT7921_LOCK(sc);
	idx = mt7921_dma_tx_get_slot(sc, &sc->sc_tx_data0, &buf, &paddr);
	if (idx < 0) {
		MT7921_UNLOCK(sc);
		return ENOBUFS;
	}

	for (i = 0; i < 8; i++) {
		uint32_t v = htole32(w[i]);
		memcpy(buf + 4 * i, &v, 4);
	}
	/* struct mt76_connac_hw_txp: msdu_id[4], ptr[2] */
	memset(buf + 32, 0, 32);
	{
		uint16_t id = htole16((uint16_t)idx | BIT(15));	/* MSDU_ID_VALID */
		uint32_t addr = htole32((uint32_t)(paddr + 64));
		uint16_t l = htole16((uint16_t)((len & 0xfff) | BIT(15)));	/* LAST */
		memcpy(buf + 32, &id, 2);
		memcpy(buf + 40, &addr, 4);			/* ptr[0].buf0 */
		memcpy(buf + 44, &l, 2);			/* ptr[0].len0 */
	}
	m_copydata(m, 0, len, (caddr_t)(buf + 64));

	mt7921_dma_tx_post(sc, &sc->sc_tx_data0, idx, 64);
	sc->sc_tx_frames++;
	if (sc->sc_tx_logged < 12) {
		sc->sc_tx_logged++;
		device_printf(sc->sc_dev, "tx: fc %02x%02x len %d wcid %d token %d\n",
			wh->i_fc[0], wh->i_fc[1], len, wcid, idx);
	}
	MT7921_UNLOCK(sc);
	return 0;
}


/* #pragma mark - joining */


void
mt7921_roc_grant_event(struct mt7921_softc* sc)
{
	sc->sc_roc_granted = true;
	wakeup(__DEVOLATILE(void*, &sc->sc_roc_granted));
}


static void
mt7921_sta_info_from_node(struct ieee80211_node* ni, bool is_5ghz,
	struct mt7921_sta_info* info)
{
	int i;

	memset(info, 0, sizeof(*info));
	IEEE80211_ADDR_COPY(info->addr, ni->ni_bssid);
	info->is_5ghz = is_5ghz;
	for (i = 0; i < ni->ni_rates.rs_nrates; i++) {
		int r = ni->ni_rates.rs_rates[i];
		int idx = mt7921_rate_index(r & IEEE80211_RATE_VAL, is_5ghz);
		if (idx < 0)
			continue;
		info->supp_rates |= 1 << idx;
		if (r & IEEE80211_RATE_BASIC)
			info->basic_rates |= 1 << idx;
	}
	if (info->basic_rates == 0)
		info->basic_rates = 1;		/* lowest rate */
	if (info->supp_rates == 0)
		info->supp_rates = info->basic_rates;
}


/*
 * After association: QoS and the negotiated 802.11n parameters, roughly
 * what mac80211 hands mt76 in sta->deflink.ht_cap.
 */
static void
mt7921_sta_info_add_ht(struct mt7921_softc* sc, struct ieee80211_node* ni,
	struct mt7921_sta_info* info)
{
	int i;

	info->qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
	if (!sc->sc_ht || (ni->ni_flags & IEEE80211_NODE_HT) == 0)
		return;

	info->ht = true;
	/* 20 MHz only: drop the 40 MHz bits. */
	info->ht_cap = ni->ni_htcap & (IEEE80211_HTCAP_LDPC | IEEE80211_HTCAP_SMPS
		| IEEE80211_HTCAP_SHORTGI20 | IEEE80211_HTCAP_TXSTBC
		| IEEE80211_HTCAP_RXSTBC);
	info->ampdu_factor = ni->ni_htparam & IEEE80211_HTCAP_MAXRXAMPDU;
	info->ampdu_density = (ni->ni_htparam & IEEE80211_HTCAP_MPDUDENSITY) >> 2;
	info->smps_dynamic = (ni->ni_htcap & IEEE80211_HTCAP_SMPS)
		== IEEE80211_HTCAP_SMPS_DYNAMIC;
	for (i = 0; i < ni->ni_htrates.rs_nrates; i++) {
		int mcs = ni->ni_htrates.rs_rates[i] & IEEE80211_RATE_VAL;
		if (mcs < 8 * (int)sizeof(info->mcs_mask))
			info->mcs_mask[mcs / 8] |= 1 << (mcs % 8);
	}
}


/* Before authenticating: station record for the AP, then go to its channel. */
static void
mt7921_prepare_join(struct mt7921_softc* sc, struct ieee80211_node* ni)
{
	struct mt7921_sta_info info;
	bool is_5ghz = IEEE80211_IS_CHAN_5GHZ(ni->ni_chan);
	uint8_t channel = ieee80211_chan2ieee(&sc->sc_ic, ni->ni_chan);
	int error, deadline;

	if (!sc->sc_can_tx)
		return;

	IEEE80211_ADDR_COPY(sc->sc_bssid, ni->ni_bssid);
	sc->sc_bss_5ghz = is_5ghz;
	sc->sc_bss_channel = channel;

	mt7921_sta_info_from_node(ni, is_5ghz, &info);
	info.enable = true;
	info.newly = true;
	info.state = MT7921_STA_STATE_NONE;
	info.aid = 0;

	MT7921_LOCK(sc);
	(void)mt7921_wtbl_update(sc, MT7921_WCID_STA);
	MT7921_UNLOCK(sc);
	error = mt7921_mcu_sta_update(sc, &info);
	if (error != 0) {
		device_printf(sc->sc_dev, "station record failed (%d)\n", error);
		return;
	}
	sc->sc_sta_added = true;

	/* mgd_prepare_tx(): remain on the AP's channel while joining. */
	sc->sc_roc_granted = false;
	error = mt7921_mcu_set_roc(sc, channel, is_5ghz, 1000);
	if (error != 0) {
		device_printf(sc->sc_dev, "channel request failed (%d)\n", error);
		return;
	}
	sc->sc_roc_active = true;

	MT7921_LOCK(sc);
	deadline = ticks + hz;
	while (!sc->sc_roc_granted && ticks - deadline < 0) {
		mt7921_mcu_poll_locked(sc);
		if (sc->sc_roc_granted)
			break;
		msleep(__DEVOLATILE(void*, &sc->sc_roc_granted), &sc->sc_mtx, 0,
			"mt7921roc", MAX(1, hz / 100));
	}
	MT7921_UNLOCK(sc);

	device_printf(sc->sc_dev, "joining " MACFMT " on channel %d: channel %s\n",
		MACARG(ni->ni_bssid), channel,
		sc->sc_roc_granted ? "granted" : "NOT granted (trying anyway)");
}


/* Associated: tell the firmware about the BSS and the AP's new state. */
static void
mt7921_join_complete(struct mt7921_softc* sc, struct ieee80211_node* ni)
{
	struct mt7921_bss_info bi;
	struct mt7921_sta_info info;
	int error;

	if (!sc->sc_can_tx || !sc->sc_sta_added)
		return;

	memset(&bi, 0, sizeof(bi));
	IEEE80211_ADDR_COPY(bi.bssid, ni->ni_bssid);
	bi.channel = ieee80211_chan2ieee(&sc->sc_ic, ni->ni_chan);
	bi.is_5ghz = sc->sc_bss_5ghz;
	bi.beacon_int = ni->ni_intval;
	bi.dtim_period = ni->ni_dtim_period;
	bi.enable = true;
	bi.qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
	bi.ht = sc->sc_ht && (ni->ni_flags & IEEE80211_NODE_HT) != 0;
	error = mt7921_mcu_bss_update(sc, &bi);
	if (error != 0)
		device_printf(sc->sc_dev, "BSS update failed (%d)\n", error);

	MT7921_LOCK(sc);
	(void)mt7921_wtbl_update(sc, MT7921_WCID_STA);
	MT7921_UNLOCK(sc);

	mt7921_sta_info_from_node(ni, sc->sc_bss_5ghz, &info);
	info.enable = true;
	info.newly = false;
	info.state = MT7921_STA_STATE_ASSOC;
	info.aid = IEEE80211_NODE_AID(ni);
	mt7921_sta_info_add_ht(sc, ni, &info);
	error = mt7921_mcu_sta_update(sc, &info);
	if (error != 0)
		device_printf(sc->sc_dev, "station update failed (%d)\n", error);

	if (sc->sc_roc_active) {
		(void)mt7921_mcu_abort_roc(sc);
		sc->sc_roc_active = false;
	}

	if (bi.qos)
		mt7921_wme_apply(sc);

	device_printf(sc->sc_dev, "associated with " MACFMT " (AID %d, %s%s)\n",
		MACARG(ni->ni_bssid), info.aid,
		info.ht ? "802.11n" : "legacy", info.qos ? ", QoS" : "");
}


/* Left the BSS (or gave up joining): remove what we told the firmware. */
static void
mt7921_leave(struct mt7921_softc* sc)
{
	struct mt7921_sta_info info;
	struct mt7921_bss_info bi;

	if (sc->sc_roc_active) {
		(void)mt7921_mcu_abort_roc(sc);
		sc->sc_roc_active = false;
	}
	if (!sc->sc_sta_added)
		return;

	/* Removing the station record ends its block-ack sessions too. */
	MT7921_LOCK(sc);
	sc->sc_ba_active = 0;
	sc->sc_ba_stop = 0;
	MT7921_UNLOCK(sc);

	memset(&info, 0, sizeof(info));
	IEEE80211_ADDR_COPY(info.addr, sc->sc_bssid);
	info.enable = false;
	(void)mt7921_mcu_sta_update(sc, &info);

	memset(&bi, 0, sizeof(bi));
	IEEE80211_ADDR_COPY(bi.bssid, sc->sc_bssid);
	bi.channel = sc->sc_bss_channel;
	bi.is_5ghz = sc->sc_bss_5ghz;
	bi.enable = false;
	(void)mt7921_mcu_bss_update(sc, &bi);

	sc->sc_sta_added = false;
	device_printf(sc->sc_dev, "left " MACFMT "\n", MACARG(sc->sc_bssid));
}


/* #pragma mark - 802.11n block ack and WME */


/*
 * The AP starts a block-ack session (ADDBA request). net80211 does the
 * bookkeeping and reordering; the firmware has to send the block acks.
 * Called from the receive path, with no locks held, so the MCU command is
 * sent right away, before net80211 answers the AP.
 */
static int
mt7921_ampdu_rx_start(struct ieee80211_node* ni, struct ieee80211_rx_ampdu* rap,
	int baparamset, int batimeout, int baseqctl)
{
	struct mt7921_softc* sc = ni->ni_ic->ic_softc;
	int tid = _IEEE80211_MASKSHIFT(baparamset, IEEE80211_BAPS_TID);
	int error;

	error = sc->sc_ampdu_rx_start(ni, rap, baparamset, batimeout, baseqctl);
	if (error != 0 || !sc->sc_sta_added || tid >= WME_NUM_TID
		|| !IEEE80211_ADDR_EQ(ni->ni_macaddr, sc->sc_bssid))
		return error;

	if (mt7921_mcu_rx_ba(sc, ni->ni_macaddr, tid, rap->rxa_start,
			rap->rxa_wnd, true) == 0) {
		MT7921_LOCK(sc);
		sc->sc_ba_active |= 1 << tid;
		sc->sc_ba_stop &= ~(1 << tid);
		MT7921_UNLOCK(sc);
		device_printf(sc->sc_dev, "block ack on for TID %d (window %d)\n",
			tid, rap->rxa_wnd);
	} else
		device_printf(sc->sc_dev, "block ack setup for TID %d failed\n", tid);
	return 0;
}


/*
 * The session ended (DELBA, or the node goes away). This can be called
 * with net80211 locks held, so the firmware is told from a task.
 */
static void
mt7921_ampdu_rx_stop(struct ieee80211_node* ni, struct ieee80211_rx_ampdu* rap)
{
	struct mt7921_softc* sc = ni->ni_ic->ic_softc;
	int tid = rap - &ni->ni_rx_ampdu[0];

	sc->sc_ampdu_rx_stop(ni, rap);

	if (tid < 0 || tid >= WME_NUM_TID
		|| !IEEE80211_ADDR_EQ(ni->ni_macaddr, sc->sc_bssid))
		return;
	MT7921_LOCK(sc);
	if ((sc->sc_ba_active & (1 << tid)) != 0) {
		sc->sc_ba_active &= ~(1 << tid);
		sc->sc_ba_stop |= 1 << tid;
		taskqueue_enqueue(sc->sc_ic.ic_tq, &sc->sc_ba_task);
	}
	MT7921_UNLOCK(sc);
}


static void
mt7921_ba_task(void* arg, int pending)
{
	struct mt7921_softc* sc = arg;
	uint8_t addr[6];
	uint16_t stop;
	bool added;
	int tid;

	MT7921_LOCK(sc);
	stop = sc->sc_ba_stop;
	sc->sc_ba_stop = 0;
	added = sc->sc_sta_added;
	IEEE80211_ADDR_COPY(addr, sc->sc_bssid);
	MT7921_UNLOCK(sc);

	if (!added)
		return;
	for (tid = 0; tid < WME_NUM_TID; tid++) {
		if ((stop & (1 << tid)) == 0)
			continue;
		(void)mt7921_mcu_rx_ba(sc, addr, tid, 0, 0, false);
		device_printf(sc->sc_dev, "block ack off for TID %d\n", tid);
	}
}


/* Port of mt7921_conf_tx() + mt7921_mcu_set_tx(): the AP's EDCA values. */
static void
mt7921_wme_apply(struct mt7921_softc* sc)
{
	struct ieee80211com* ic = &sc->sc_ic;
	struct chanAccParams chp;
	struct mt7921_edca edca[4];
	/* mt76 order is BK, BE, VI, VO; net80211's is BE, BK, VI, VO. */
	static const int to_ac[4] = { WME_AC_BK, WME_AC_BE, WME_AC_VI, WME_AC_VO };
	int i;

	if (!sc->sc_can_tx || !sc->sc_sta_added)
		return;

	ieee80211_wme_ic_getparams(ic, &chp);
	memset(edca, 0, sizeof(edca));
	for (i = 0; i < 4; i++) {
		const struct wmeParams* p = &chp.cap_wmeParams[to_ac[i]];
		edca[i].aifs = p->wmep_aifsn;
		/* Upstream passes mac80211's plain values (2^n - 1). */
		edca[i].cw_min = (1U << p->wmep_logcwmin) - 1;
		edca[i].cw_max = (1U << p->wmep_logcwmax) - 1;
		edca[i].txop = p->wmep_txopLimit;
	}
	if (mt7921_mcu_set_edca(sc, edca, true) != 0)
		device_printf(sc->sc_dev, "EDCA update failed\n");
}


static void
mt7921_wme_task(void* arg, int pending)
{
	mt7921_wme_apply(arg);
}


/* net80211 calls this with its lock held: send the command from a task. */
static int
mt7921_wme_update(struct ieee80211com* ic)
{
	struct mt7921_softc* sc = ic->ic_softc;

	taskqueue_enqueue(ic->ic_tq, &sc->sc_wme_task);
	return 0;
}


/* #pragma mark - VAP */


static int
mt7921_newstate(struct ieee80211vap* vap, enum ieee80211_state nstate, int arg)
{
	struct mt7921_vap* mvp = MT7921_VAP(vap);
	struct ieee80211com* ic = vap->iv_ic;
	struct mt7921_softc* sc = ic->ic_softc;
	enum ieee80211_state ostate = vap->iv_state;
	struct ieee80211_node* ni;

	if (sc->sc_state_logged < 40) {
		sc->sc_state_logged++;
		device_printf(sc->sc_dev, "net80211 state %s -> %s\n",
			ieee80211_state_name[ostate], ieee80211_state_name[nstate]);
	}

	/* Talk to the firmware without holding the net80211 lock (as iwn). */
	ni = ieee80211_ref_node(vap->iv_bss);
	IEEE80211_UNLOCK(ic);

	if (ostate >= IEEE80211_S_AUTH && nstate < IEEE80211_S_AUTH)
		mt7921_leave(sc);
	else if (nstate == IEEE80211_S_AUTH
		&& (ostate < IEEE80211_S_AUTH || !sc->sc_sta_added
			|| !IEEE80211_ADDR_EQ(ni->ni_bssid, sc->sc_bssid))) {
		if (sc->sc_sta_added)
			mt7921_leave(sc);
		mt7921_prepare_join(sc, ni);
	} else if (nstate == IEEE80211_S_RUN && ostate == IEEE80211_S_ASSOC)
		mt7921_join_complete(sc, ni);

	IEEE80211_LOCK(ic);
	ieee80211_free_node(ni);

	return mvp->mv_newstate(vap, nstate, arg);
}


static struct ieee80211vap*
mt7921_vap_create(struct ieee80211com* ic, const char name[IFNAMSIZ],
	int unit, enum ieee80211_opmode opmode, int flags,
	const uint8_t bssid[IEEE80211_ADDR_LEN],
	const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct mt7921_softc* sc = ic->ic_softc;
	struct mt7921_vap* mvp;
	struct ieee80211vap* vap;

	if (!TAILQ_EMPTY(&ic->ic_vaps))		/* only one at a time */
		return NULL;
	if (opmode != IEEE80211_M_STA)
		return NULL;

	mvp = malloc(sizeof(struct mt7921_vap), M_80211_VAP, M_WAITOK | M_ZERO);
	if (mvp == NULL)
		return NULL;
	vap = &mvp->mv_vap;

	ieee80211_vap_setup(ic, vap, name, unit, opmode, flags, bssid);

	/* The firmware scans; net80211 only collects results. */
	vap->iv_flags_ext |= IEEE80211_FEXT_SCAN_OFFLOAD;

	/* What mt76 advertises: 64 KB A-MPDUs, 4 us MPDU spacing. */
	vap->iv_ampdu_rxmax = IEEE80211_HTCAP_MAXRXAMPDU_64K;
	vap->iv_ampdu_density = IEEE80211_HTCAP_MPDUDENSITY_4;

	mvp->mv_newstate = vap->iv_newstate;
	vap->iv_newstate = mt7921_newstate;

	/* The firmware does rate control; net80211 keeps the "none" algorithm. */
	ieee80211_vap_attach(vap, ieee80211_media_change, ieee80211_media_status,
		mac);
	ic->ic_opmode = opmode;

	device_printf(sc->sc_dev, "WiFi interface created\n");
	return vap;
}


static void
mt7921_vap_delete(struct ieee80211vap* vap)
{
	struct mt7921_vap* mvp = MT7921_VAP(vap);

	ieee80211_vap_detach(vap);
	free(mvp, M_80211_VAP);
}


/* #pragma mark - device hooks */


static void
mt7921_parent(struct ieee80211com* ic)
{
	struct mt7921_softc* sc = ic->ic_softc;

	if (ic->ic_nrunning > 0) {
		mt7921_hw_resume(sc);
		ieee80211_start_all(ic);
	} else
		mt7921_hw_pause(sc);

	if (!sc->sc_parent_logged) {
		sc->sc_parent_logged = true;
		device_printf(sc->sc_dev, "interface %s\n",
			ic->ic_nrunning > 0 ? "up" : "down");
	}
}


/* Data frames. On error the caller keeps (and frees) the mbuf and node. */
static int
mt7921_transmit(struct ieee80211com* ic, struct mbuf* m)
{
	struct mt7921_softc* sc = ic->ic_softc;
	struct ieee80211_node* ni = (struct ieee80211_node*)m->m_pkthdr.rcvif;
	int error;

	error = mt7921_tx(sc, ni, m);
	if (error != 0)
		return error;
	/* Copied into DMA memory: done with the mbuf and node reference. */
	ieee80211_tx_complete(ni, m, 0);
	return 0;
}


/* Management/raw frames. On error the driver frees the mbuf (not the node). */
static int
mt7921_raw_xmit(struct ieee80211_node* ni, struct mbuf* m,
	const struct ieee80211_bpf_params* params)
{
	struct mt7921_softc* sc = ni->ni_ic->ic_softc;
	int error;

	error = mt7921_tx(sc, ni, m);
	if (error != 0) {
		m_freem(m);
		return error;
	}
	ieee80211_tx_complete(ni, m, 0);
	return 0;
}


/* #pragma mark - attach */


int
mt7921_net_attach(struct mt7921_softc* sc)
{
	struct ieee80211com* ic = &sc->sc_ic;

	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(sc->sc_dev);
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_caps = IEEE80211_C_STA
		| IEEE80211_C_WPA		/* software crypto in net80211 for now */
		| IEEE80211_C_SHPREAMBLE
		| IEEE80211_C_SHSLOT;
	if (sc->sc_ht) {
		ic->ic_caps |= IEEE80211_C_WME;
		/*
		 * Two streams, 20 MHz. No TX aggregation yet (net80211 would
		 * need the firmware's TX block-ack commands), and no A-MSDU
		 * inside A-MPDU, so the AP sends at most a 3839-byte A-MSDU.
		 */
		ic->ic_htcaps = IEEE80211_HTC_HT
			| IEEE80211_HTCAP_SMPS_OFF
			| IEEE80211_HTCAP_SHORTGI20
			| IEEE80211_HTCAP_LDPC
			| IEEE80211_HTCAP_TXSTBC
			| IEEE80211_HTCAP_RXSTBC_1STREAM;
		ic->ic_txstream = 2;
		ic->ic_rxstream = 2;
	}
	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->sc_macaddr);

	mt7921_getradiocaps(ic, IEEE80211_CHAN_MAX, &ic->ic_nchans,
		ic->ic_channels);

	mbufq_init(&sc->sc_rxq, 256);
	TASK_INIT(&sc->sc_rx_task, 0, mt7921_rx_task, sc);
	TASK_INIT(&sc->sc_scan_done_task, 0, mt7921_scan_done_task, sc);
	TASK_INIT(&sc->sc_ba_task, 0, mt7921_ba_task, sc);
	TASK_INIT(&sc->sc_wme_task, 0, mt7921_wme_task, sc);
	callout_init_mtx(&sc->sc_scan_to, &sc->sc_mtx, 0);

	ieee80211_ifattach(ic);

	ic->ic_vap_create = mt7921_vap_create;
	ic->ic_vap_delete = mt7921_vap_delete;
	ic->ic_parent = mt7921_parent;
	ic->ic_transmit = mt7921_transmit;
	ic->ic_raw_xmit = mt7921_raw_xmit;
	ic->ic_scan_start = mt7921_scan_start;
	ic->ic_scan_end = mt7921_scan_end;
	ic->ic_set_channel = mt7921_set_channel;
	ic->ic_getradiocaps = mt7921_getradiocaps;
	/* Set after ieee80211_ifattach(), which installs the defaults. */
	ic->ic_scan_curchan = mt7921_scan_curchan;
	ic->ic_scan_mindwell = mt7921_scan_mindwell;
	if (sc->sc_ht) {
		ic->ic_wme.wme_update = mt7921_wme_update;
		sc->sc_ampdu_rx_start = ic->ic_ampdu_rx_start;
		sc->sc_ampdu_rx_stop = ic->ic_ampdu_rx_stop;
		ic->ic_ampdu_rx_start = mt7921_ampdu_rx_start;
		ic->ic_ampdu_rx_stop = mt7921_ampdu_rx_stop;
	}

	sc->sc_ic_attached = true;
	device_printf(sc->sc_dev, "registered with net80211 (%d channels)\n",
		ic->ic_nchans);
	return 0;
}


/* Interrupts must already be torn down, so nothing new gets queued. */
void
mt7921_net_detach(struct mt7921_softc* sc)
{
	struct ieee80211com* ic = &sc->sc_ic;

	if (!sc->sc_ic_attached)
		return;

	callout_drain(&sc->sc_scan_to);
	taskqueue_drain(ic->ic_tq, &sc->sc_rx_task);
	taskqueue_drain(ic->ic_tq, &sc->sc_scan_done_task);
	taskqueue_drain(ic->ic_tq, &sc->sc_ba_task);
	taskqueue_drain(ic->ic_tq, &sc->sc_wme_task);

	MT7921_LOCK(sc);
	sc->sc_ic_attached = false;
	MT7921_UNLOCK(sc);

	ieee80211_ifdetach(ic);
	mbufq_drain(&sc->sc_rxq);
}
