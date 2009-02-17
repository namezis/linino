/**
  * This file contains the handling of command
  * responses as well as events generated by firmware.
  */
#include <linux/delay.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>

#include <net/iw_handler.h>

#include "host.h"
#include "decl.h"
#include "defs.h"
#include "dev.h"
#include "join.h"
#include "wext.h"

/**
 *  @brief This function handles disconnect event. it
 *  reports disconnect to upper layer, clean tx/rx packets,
 *  reset link state etc.
 *
 *  @param priv    A pointer to struct lbs_private structure
 *  @return 	   n/a
 */
void lbs_mac_event_disconnected(struct lbs_private *priv)
{
	union iwreq_data wrqu;

	if (priv->connect_status != LBS_CONNECTED)
		return;

	lbs_deb_enter(LBS_DEB_ASSOC);

	memset(wrqu.ap_addr.sa_data, 0x00, ETH_ALEN);
	wrqu.ap_addr.sa_family = ARPHRD_ETHER;

	/*
	 * Cisco AP sends EAP failure and de-auth in less than 0.5 ms.
	 * It causes problem in the Supplicant
	 */

	msleep_interruptible(1000);
	wireless_send_event(priv->dev, SIOCGIWAP, &wrqu, NULL);

	/* report disconnect to upper layer */
	netif_stop_queue(priv->dev);
	netif_carrier_off(priv->dev);

	/* Free Tx and Rx packets */
	kfree_skb(priv->currenttxskb);
	priv->currenttxskb = NULL;
	priv->tx_pending_len = 0;

	/* reset SNR/NF/RSSI values */
	memset(priv->SNR, 0x00, sizeof(priv->SNR));
	memset(priv->NF, 0x00, sizeof(priv->NF));
	memset(priv->RSSI, 0x00, sizeof(priv->RSSI));
	memset(priv->rawSNR, 0x00, sizeof(priv->rawSNR));
	memset(priv->rawNF, 0x00, sizeof(priv->rawNF));
	priv->nextSNRNF = 0;
	priv->numSNRNF = 0;
	priv->connect_status = LBS_DISCONNECTED;

	/* Clear out associated SSID and BSSID since connection is
	 * no longer valid.
	 */
	memset(&priv->curbssparams.bssid, 0, ETH_ALEN);
	memset(&priv->curbssparams.ssid, 0, IW_ESSID_MAX_SIZE);
	priv->curbssparams.ssid_len = 0;

	if (priv->psstate != PS_STATE_FULL_POWER) {
		/* make firmware to exit PS mode */
		lbs_deb_cmd("disconnected, so exit PS mode\n");
		lbs_ps_wakeup(priv, 0);
	}
	lbs_deb_leave(LBS_DEB_CMD);
}

/**
 *  @brief This function handles MIC failure event.
 *
 *  @param priv    A pointer to struct lbs_private structure
 *  @para  event   the event id
 *  @return 	   n/a
 */
static void handle_mic_failureevent(struct lbs_private *priv, u32 event)
{
	char buf[50];

	lbs_deb_enter(LBS_DEB_CMD);
	memset(buf, 0, sizeof(buf));

	sprintf(buf, "%s", "MLME-MICHAELMICFAILURE.indication ");

	if (event == MACREG_INT_CODE_MIC_ERR_UNICAST) {
		strcat(buf, "unicast ");
	} else {
		strcat(buf, "multicast ");
	}

	lbs_send_iwevcustom_event(priv, buf);
	lbs_deb_leave(LBS_DEB_CMD);
}

static int lbs_ret_reg_access(struct lbs_private *priv,
			       u16 type, struct cmd_ds_command *resp)
{
	int ret = 0;

	lbs_deb_enter(LBS_DEB_CMD);

	switch (type) {
	case CMD_RET(CMD_MAC_REG_ACCESS):
		{
			struct cmd_ds_mac_reg_access *reg = &resp->params.macreg;

			priv->offsetvalue.offset = (u32)le16_to_cpu(reg->offset);
			priv->offsetvalue.value = le32_to_cpu(reg->value);
			break;
		}

	case CMD_RET(CMD_BBP_REG_ACCESS):
		{
			struct cmd_ds_bbp_reg_access *reg = &resp->params.bbpreg;

			priv->offsetvalue.offset = (u32)le16_to_cpu(reg->offset);
			priv->offsetvalue.value = reg->value;
			break;
		}

	case CMD_RET(CMD_RF_REG_ACCESS):
		{
			struct cmd_ds_rf_reg_access *reg = &resp->params.rfreg;

			priv->offsetvalue.offset = (u32)le16_to_cpu(reg->offset);
			priv->offsetvalue.value = reg->value;
			break;
		}

	default:
		ret = -1;
	}

	lbs_deb_leave_args(LBS_DEB_CMD, "ret %d", ret);
	return ret;
}

static int lbs_ret_802_11_stat(struct lbs_private *priv,
				struct cmd_ds_command *resp)
{
	lbs_deb_enter(LBS_DEB_CMD);
/*	currently priv->wlan802_11Stat is unused

	struct cmd_ds_802_11_get_stat *p11Stat = &resp->params.gstat;

	// TODO Convert it to Big endian befor copy
	memcpy(&priv->wlan802_11Stat,
	       p11Stat, sizeof(struct cmd_ds_802_11_get_stat));
*/
	lbs_deb_leave(LBS_DEB_CMD);
	return 0;
}

static int lbs_ret_802_11_snmp_mib(struct lbs_private *priv,
				    struct cmd_ds_command *resp)
{
	struct cmd_ds_802_11_snmp_mib *smib = &resp->params.smib;
	u16 oid = le16_to_cpu(smib->oid);
	u16 querytype = le16_to_cpu(smib->querytype);

	lbs_deb_enter(LBS_DEB_CMD);

	lbs_deb_cmd("SNMP_RESP: oid 0x%x, querytype 0x%x\n", oid,
	       querytype);
	lbs_deb_cmd("SNMP_RESP: Buf size %d\n", le16_to_cpu(smib->bufsize));

	if (querytype == CMD_ACT_GET) {
		switch (oid) {
		case FRAGTHRESH_I:
			priv->fragthsd =
				le16_to_cpu(*((__le16 *)(smib->value)));
			lbs_deb_cmd("SNMP_RESP: frag threshold %u\n",
				    priv->fragthsd);
			break;
		case RTSTHRESH_I:
			priv->rtsthsd =
				le16_to_cpu(*((__le16 *)(smib->value)));
			lbs_deb_cmd("SNMP_RESP: rts threshold %u\n",
				    priv->rtsthsd);
			break;
		case SHORT_RETRYLIM_I:
			priv->txretrycount =
				le16_to_cpu(*((__le16 *)(smib->value)));
			lbs_deb_cmd("SNMP_RESP: tx retry count %u\n",
				    priv->rtsthsd);
			break;
		default:
			break;
		}
	}

	lbs_deb_enter(LBS_DEB_CMD);
	return 0;
}

static int lbs_ret_802_11_key_material(struct lbs_private *priv,
					struct cmd_ds_command *resp)
{
	struct cmd_ds_802_11_key_material *pkeymaterial =
	    &resp->params.keymaterial;
	u16 action = le16_to_cpu(pkeymaterial->action);

	lbs_deb_enter(LBS_DEB_CMD);

	/* Copy the returned key to driver private data */
	if (action == CMD_ACT_GET) {
		u8 * buf_ptr = (u8 *) &pkeymaterial->keyParamSet;
		u8 * resp_end = (u8 *) (resp + le16_to_cpu(resp->size));

		while (buf_ptr < resp_end) {
			struct MrvlIEtype_keyParamSet * pkeyparamset =
			    (struct MrvlIEtype_keyParamSet *) buf_ptr;
			struct enc_key * pkey;
			u16 param_set_len = le16_to_cpu(pkeyparamset->length);
			u16 key_len = le16_to_cpu(pkeyparamset->keylen);
			u16 key_flags = le16_to_cpu(pkeyparamset->keyinfo);
			u16 key_type = le16_to_cpu(pkeyparamset->keytypeid);
			u8 * end;

			end = (u8 *) pkeyparamset + sizeof (pkeyparamset->type)
			                          + sizeof (pkeyparamset->length)
			                          + param_set_len;
			/* Make sure we don't access past the end of the IEs */
			if (end > resp_end)
				break;

			if (key_flags & KEY_INFO_WPA_UNICAST)
				pkey = &priv->wpa_unicast_key;
			else if (key_flags & KEY_INFO_WPA_MCAST)
				pkey = &priv->wpa_mcast_key;
			else
				break;

			/* Copy returned key into driver */
			memset(pkey, 0, sizeof(struct enc_key));
			if (key_len > sizeof(pkey->key))
				break;
			pkey->type = key_type;
			pkey->flags = key_flags;
			pkey->len = key_len;
			memcpy(pkey->key, pkeyparamset->key, pkey->len);

			buf_ptr = end + 1;
		}
	}

	lbs_deb_enter(LBS_DEB_CMD);
	return 0;
}

static int lbs_ret_802_11_mac_address(struct lbs_private *priv,
				       struct cmd_ds_command *resp)
{
	struct cmd_ds_802_11_mac_address *macadd = &resp->params.macadd;

	lbs_deb_enter(LBS_DEB_CMD);

	memcpy(priv->current_addr, macadd->macadd, ETH_ALEN);

	lbs_deb_enter(LBS_DEB_CMD);
	return 0;
}

static int lbs_ret_802_11_rf_tx_power(struct lbs_private *priv,
				       struct cmd_ds_command *resp)
{
	struct cmd_ds_802_11_rf_tx_power *rtp = &resp->params.txp;

	lbs_deb_enter(LBS_DEB_CMD);

	priv->txpowerlevel = le16_to_cpu(rtp->currentlevel);

	lbs_deb_cmd("TX power currently %d\n", priv->txpowerlevel);

	lbs_deb_leave(LBS_DEB_CMD);
	return 0;
}

static int lbs_ret_802_11_rate_adapt_rateset(struct lbs_private *priv,
					      struct cmd_ds_command *resp)
{
	struct cmd_ds_802_11_rate_adapt_rateset *rates = &resp->params.rateset;

	lbs_deb_enter(LBS_DEB_CMD);

	if (rates->action == CMD_ACT_GET) {
		priv->enablehwauto = le16_to_cpu(rates->enablehwauto);
		priv->ratebitmap = le16_to_cpu(rates->bitmap);
	}

	lbs_deb_leave(LBS_DEB_CMD);
	return 0;
}

static int lbs_ret_802_11_rssi(struct lbs_private *priv,
				struct cmd_ds_command *resp)
{
	struct cmd_ds_802_11_rssi_rsp *rssirsp = &resp->params.rssirsp;

	lbs_deb_enter(LBS_DEB_CMD);

	/* store the non average value */
	priv->SNR[TYPE_BEACON][TYPE_NOAVG] = le16_to_cpu(rssirsp->SNR);
	priv->NF[TYPE_BEACON][TYPE_NOAVG] = le16_to_cpu(rssirsp->noisefloor);

	priv->SNR[TYPE_BEACON][TYPE_AVG] = le16_to_cpu(rssirsp->avgSNR);
	priv->NF[TYPE_BEACON][TYPE_AVG] = le16_to_cpu(rssirsp->avgnoisefloor);

	priv->RSSI[TYPE_BEACON][TYPE_NOAVG] =
	    CAL_RSSI(priv->SNR[TYPE_BEACON][TYPE_NOAVG],
		     priv->NF[TYPE_BEACON][TYPE_NOAVG]);

	priv->RSSI[TYPE_BEACON][TYPE_AVG] =
	    CAL_RSSI(priv->SNR[TYPE_BEACON][TYPE_AVG] / AVG_SCALE,
		     priv->NF[TYPE_BEACON][TYPE_AVG] / AVG_SCALE);

	lbs_deb_cmd("RSSI: beacon %d, avg %d\n",
	       priv->RSSI[TYPE_BEACON][TYPE_NOAVG],
	       priv->RSSI[TYPE_BEACON][TYPE_AVG]);

	lbs_deb_leave(LBS_DEB_CMD);
	return 0;
}

static int lbs_ret_802_11_eeprom_access(struct lbs_private *priv,
				  struct cmd_ds_command *resp)
{
	struct lbs_ioctl_regrdwr *pbuf;
	pbuf = (struct lbs_ioctl_regrdwr *) priv->prdeeprom;

	lbs_deb_enter_args(LBS_DEB_CMD, "len %d",
	       le16_to_cpu(resp->params.rdeeprom.bytecount));
	if (pbuf->NOB < le16_to_cpu(resp->params.rdeeprom.bytecount)) {
		pbuf->NOB = 0;
		lbs_deb_cmd("EEPROM read length too big\n");
		return -1;
	}
	pbuf->NOB = le16_to_cpu(resp->params.rdeeprom.bytecount);
	if (pbuf->NOB > 0) {

		memcpy(&pbuf->value, (u8 *) & resp->params.rdeeprom.value,
		       le16_to_cpu(resp->params.rdeeprom.bytecount));
		lbs_deb_hex(LBS_DEB_CMD, "EEPROM", (char *)&pbuf->value,
			le16_to_cpu(resp->params.rdeeprom.bytecount));
	}
	lbs_deb_leave(LBS_DEB_CMD);
	return 0;
}

static int lbs_ret_get_log(struct lbs_private *priv,
			    struct cmd_ds_command *resp)
{
	struct cmd_ds_802_11_get_log *logmessage = &resp->params.glog;

	lbs_deb_enter(LBS_DEB_CMD);

	/* Stored little-endian */
	memcpy(&priv->logmsg, logmessage, sizeof(struct cmd_ds_802_11_get_log));

	lbs_deb_leave(LBS_DEB_CMD);
	return 0;
}

static int lbs_ret_802_11_bcn_ctrl(struct lbs_private * priv,
					struct cmd_ds_command *resp)
{
	struct cmd_ds_802_11_beacon_control *bcn_ctrl =
	    &resp->params.bcn_ctrl;

	lbs_deb_enter(LBS_DEB_CMD);

	if (bcn_ctrl->action == CMD_ACT_GET) {
		priv->beacon_enable = (u8) le16_to_cpu(bcn_ctrl->beacon_enable);
		priv->beacon_period = le16_to_cpu(bcn_ctrl->beacon_period);
	}

	lbs_deb_enter(LBS_DEB_CMD);
	return 0;
}

static inline int handle_cmd_response(struct lbs_private *priv,
				      unsigned long dummy,
				      struct cmd_header *cmd_response)
{
	struct cmd_ds_command *resp = (struct cmd_ds_command *) cmd_response;
	int ret = 0;
	unsigned long flags;
	uint16_t respcmd = le16_to_cpu(resp->command);

	lbs_deb_enter(LBS_DEB_HOST);

	switch (respcmd) {
	case CMD_RET(CMD_MAC_REG_ACCESS):
	case CMD_RET(CMD_BBP_REG_ACCESS):
	case CMD_RET(CMD_RF_REG_ACCESS):
		ret = lbs_ret_reg_access(priv, respcmd, resp);
		break;

	case CMD_RET(CMD_802_11_SCAN):
		ret = lbs_ret_80211_scan(priv, resp);
		break;

	case CMD_RET(CMD_802_11_GET_LOG):
		ret = lbs_ret_get_log(priv, resp);
		break;

	case CMD_RET_802_11_ASSOCIATE:
	case CMD_RET(CMD_802_11_ASSOCIATE):
	case CMD_RET(CMD_802_11_REASSOCIATE):
		ret = lbs_ret_80211_associate(priv, resp);
		break;

	case CMD_RET(CMD_802_11_DISASSOCIATE):
	case CMD_RET(CMD_802_11_DEAUTHENTICATE):
		ret = lbs_ret_80211_disassociate(priv, resp);
		break;

	case CMD_RET(CMD_802_11_AD_HOC_START):
	case CMD_RET(CMD_802_11_AD_HOC_JOIN):
		ret = lbs_ret_80211_ad_hoc_start(priv, resp);
		break;

	case CMD_RET(CMD_802_11_GET_STAT):
		ret = lbs_ret_802_11_stat(priv, resp);
		break;

	case CMD_RET(CMD_802_11_SNMP_MIB):
		ret = lbs_ret_802_11_snmp_mib(priv, resp);
		break;

	case CMD_RET(CMD_802_11_RF_TX_POWER):
		ret = lbs_ret_802_11_rf_tx_power(priv, resp);
		break;

	case CMD_RET(CMD_802_11_SET_AFC):
	case CMD_RET(CMD_802_11_GET_AFC):
		spin_lock_irqsave(&priv->driver_lock, flags);
		memmove((void *)priv->cur_cmd->callback_arg, &resp->params.afc,
			sizeof(struct cmd_ds_802_11_afc));
		spin_unlock_irqrestore(&priv->driver_lock, flags);

		break;

	case CMD_RET(CMD_MAC_MULTICAST_ADR):
	case CMD_RET(CMD_MAC_CONTROL):
	case CMD_RET(CMD_802_11_RESET):
	case CMD_RET(CMD_802_11_AUTHENTICATE):
	case CMD_RET(CMD_802_11_BEACON_STOP):
		break;

	case CMD_RET(CMD_802_11_RATE_ADAPT_RATESET):
		ret = lbs_ret_802_11_rate_adapt_rateset(priv, resp);
		break;

	case CMD_RET(CMD_802_11_RSSI):
		ret = lbs_ret_802_11_rssi(priv, resp);
		break;

	case CMD_RET(CMD_802_11_MAC_ADDRESS):
		ret = lbs_ret_802_11_mac_address(priv, resp);
		break;

	case CMD_RET(CMD_802_11_AD_HOC_STOP):
		ret = lbs_ret_80211_ad_hoc_stop(priv, resp);
		break;

	case CMD_RET(CMD_802_11_KEY_MATERIAL):
		ret = lbs_ret_802_11_key_material(priv, resp);
		break;

	case CMD_RET(CMD_802_11_EEPROM_ACCESS):
		ret = lbs_ret_802_11_eeprom_access(priv, resp);
		break;

	case CMD_RET(CMD_802_11D_DOMAIN_INFO):
		ret = lbs_ret_802_11d_domain_info(priv, resp);
		break;

	case CMD_RET(CMD_802_11_TPC_CFG):
		spin_lock_irqsave(&priv->driver_lock, flags);
		memmove((void *)priv->cur_cmd->callback_arg, &resp->params.tpccfg,
			sizeof(struct cmd_ds_802_11_tpc_cfg));
		spin_unlock_irqrestore(&priv->driver_lock, flags);
		break;
	case CMD_RET(CMD_802_11_LED_GPIO_CTRL):
		spin_lock_irqsave(&priv->driver_lock, flags);
		memmove((void *)priv->cur_cmd->callback_arg, &resp->params.ledgpio,
			sizeof(struct cmd_ds_802_11_led_ctrl));
		spin_unlock_irqrestore(&priv->driver_lock, flags);
		break;

	case CMD_RET(CMD_802_11_PWR_CFG):
		spin_lock_irqsave(&priv->driver_lock, flags);
		memmove((void *)priv->cur_cmd->callback_arg, &resp->params.pwrcfg,
			sizeof(struct cmd_ds_802_11_pwr_cfg));
		spin_unlock_irqrestore(&priv->driver_lock, flags);

		break;

	case CMD_RET(CMD_GET_TSF):
		spin_lock_irqsave(&priv->driver_lock, flags);
		memcpy((void *)priv->cur_cmd->callback_arg,
		       &resp->params.gettsf.tsfvalue, sizeof(u64));
		spin_unlock_irqrestore(&priv->driver_lock, flags);
		break;
	case CMD_RET(CMD_BT_ACCESS):
		spin_lock_irqsave(&priv->driver_lock, flags);
		if (priv->cur_cmd->callback_arg)
			memcpy((void *)priv->cur_cmd->callback_arg,
			       &resp->params.bt.addr1, 2 * ETH_ALEN);
		spin_unlock_irqrestore(&priv->driver_lock, flags);
		break;
	case CMD_RET(CMD_FWT_ACCESS):
		spin_lock_irqsave(&priv->driver_lock, flags);
		if (priv->cur_cmd->callback_arg)
			memcpy((void *)priv->cur_cmd->callback_arg, &resp->params.fwt,
			       sizeof(resp->params.fwt));
		spin_unlock_irqrestore(&priv->driver_lock, flags);
		break;
	case CMD_RET(CMD_802_11_BEACON_CTRL):
		ret = lbs_ret_802_11_bcn_ctrl(priv, resp);
		break;

	default:
		lbs_deb_host("CMD_RESP: unknown cmd response 0x%04x\n",
			     le16_to_cpu(resp->command));
		break;
	}
	lbs_deb_leave(LBS_DEB_HOST);
	return ret;
}

int lbs_process_rx_command(struct lbs_private *priv)
{
	uint16_t respcmd, curcmd;
	struct cmd_header *resp;
	int ret = 0;
	unsigned long flags;
	uint16_t result;

	lbs_deb_enter(LBS_DEB_HOST);

	mutex_lock(&priv->lock);
	spin_lock_irqsave(&priv->driver_lock, flags);

	if (!priv->cur_cmd) {
		lbs_deb_host("CMD_RESP: cur_cmd is NULL\n");
		ret = -1;
		spin_unlock_irqrestore(&priv->driver_lock, flags);
		goto done;
	}

	resp = (void *)priv->upld_buf;

	curcmd = le16_to_cpu(resp->command);

	respcmd = le16_to_cpu(resp->command);
	result = le16_to_cpu(resp->result);

	lbs_deb_host("CMD_RESP: response 0x%04x, seq %d, size %d, jiffies %lu\n",
		     respcmd, le16_to_cpu(resp->seqnum), priv->upld_len, jiffies);
	lbs_deb_hex(LBS_DEB_HOST, "CMD_RESP", (void *) resp, priv->upld_len);

	if (resp->seqnum != resp->seqnum) {
		lbs_pr_info("Received CMD_RESP with invalid sequence %d (expected %d)\n",
			    le16_to_cpu(resp->seqnum), le16_to_cpu(resp->seqnum));
		spin_unlock_irqrestore(&priv->driver_lock, flags);
		ret = -1;
		goto done;
	}
	if (respcmd != CMD_RET(curcmd) &&
	    respcmd != CMD_802_11_ASSOCIATE && curcmd != CMD_RET_802_11_ASSOCIATE) {
		lbs_pr_info("Invalid CMD_RESP %x to command %x!\n", respcmd, curcmd);
		spin_unlock_irqrestore(&priv->driver_lock, flags);
		ret = -1;
		goto done;
	}

	if (resp->result == cpu_to_le16(0x0004)) {
		/* 0x0004 means -EAGAIN. Drop the response, let it time out
		   and be resubmitted */
		lbs_pr_info("Firmware returns DEFER to command %x. Will let it time out...\n",
			    le16_to_cpu(resp->command));
		spin_unlock_irqrestore(&priv->driver_lock, flags);
		ret = -1;
		goto done;
	}

	/* Now we got response from FW, cancel the command timer */
	del_timer(&priv->command_timer);
	priv->cmd_timed_out = 0;
	if (priv->nr_retries) {
		lbs_pr_info("Received result %x to command %x after %d retries\n",
			    result, curcmd, priv->nr_retries);
		priv->nr_retries = 0;
	}

	/* Store the response code to cur_cmd_retcode. */
	priv->cur_cmd_retcode = result;

	if (respcmd == CMD_RET(CMD_802_11_PS_MODE)) {
		struct cmd_ds_802_11_ps_mode *psmode = (void *) &resp[1];
		u16 action = le16_to_cpu(psmode->action);

		lbs_deb_host(
		       "CMD_RESP: PS_MODE cmd reply result 0x%x, action 0x%x\n",
		       result, action);

		if (result) {
			lbs_deb_host("CMD_RESP: PS command failed with 0x%x\n",
				    result);
			/*
			 * We should not re-try enter-ps command in
			 * ad-hoc mode. It takes place in
			 * lbs_execute_next_command().
			 */
			if (priv->mode == IW_MODE_ADHOC &&
			    action == CMD_SUBCMD_ENTER_PS)
				priv->psmode = LBS802_11POWERMODECAM;
		} else if (action == CMD_SUBCMD_ENTER_PS) {
			priv->needtowakeup = 0;
			priv->psstate = PS_STATE_AWAKE;

			lbs_deb_host("CMD_RESP: ENTER_PS command response\n");
			if (priv->connect_status != LBS_CONNECTED) {
				/*
				 * When Deauth Event received before Enter_PS command
				 * response, We need to wake up the firmware.
				 */
				lbs_deb_host(
				       "disconnected, invoking lbs_ps_wakeup\n");

				spin_unlock_irqrestore(&priv->driver_lock, flags);
				mutex_unlock(&priv->lock);
				lbs_ps_wakeup(priv, 0);
				mutex_lock(&priv->lock);
				spin_lock_irqsave(&priv->driver_lock, flags);
			}
		} else if (action == CMD_SUBCMD_EXIT_PS) {
			priv->needtowakeup = 0;
			priv->psstate = PS_STATE_FULL_POWER;
			lbs_deb_host("CMD_RESP: EXIT_PS command response\n");
		} else {
			lbs_deb_host("CMD_RESP: PS action 0x%X\n", action);
		}

		lbs_complete_command(priv, priv->cur_cmd, result);
		spin_unlock_irqrestore(&priv->driver_lock, flags);

		ret = 0;
		goto done;
	}

	/* If the command is not successful, cleanup and return failure */
	if ((result != 0 || !(respcmd & 0x8000))) {
		lbs_deb_host("CMD_RESP: error 0x%04x in command reply 0x%04x\n",
		       result, respcmd);
		/*
		 * Handling errors here
		 */
		switch (respcmd) {
		case CMD_RET(CMD_GET_HW_SPEC):
		case CMD_RET(CMD_802_11_RESET):
			lbs_deb_host("CMD_RESP: reset failed\n");
			break;

		}
		lbs_complete_command(priv, priv->cur_cmd, result);
		spin_unlock_irqrestore(&priv->driver_lock, flags);

		ret = -1;
		goto done;
	}

	spin_unlock_irqrestore(&priv->driver_lock, flags);

	if (priv->cur_cmd && priv->cur_cmd->callback) {
		ret = priv->cur_cmd->callback(priv, priv->cur_cmd->callback_arg,
				resp);
	} else
		ret = handle_cmd_response(priv, 0, resp);

	spin_lock_irqsave(&priv->driver_lock, flags);

	if (priv->cur_cmd) {
		/* Clean up and Put current command back to cmdfreeq */
		lbs_complete_command(priv, priv->cur_cmd, result);
	}
	spin_unlock_irqrestore(&priv->driver_lock, flags);

done:
	mutex_unlock(&priv->lock);
	lbs_deb_leave_args(LBS_DEB_HOST, "ret %d", ret);
	return ret;
}

static int lbs_send_confirmwake(struct lbs_private *priv)
{
	struct cmd_header *cmd = &priv->lbs_ps_confirm_wake;
	int ret = 0;

	lbs_deb_enter(LBS_DEB_HOST);

	cmd->command = cpu_to_le16(CMD_802_11_WAKEUP_CONFIRM);
	cmd->size = cpu_to_le16(sizeof(*cmd));
	cmd->seqnum = cpu_to_le16(++priv->seqnum);
	cmd->result = 0;

	lbs_deb_host("SEND_WAKEC_CMD: before download\n");

	lbs_deb_hex(LBS_DEB_HOST, "wake confirm command", (void *)cmd, sizeof(*cmd));

	ret = priv->hw_host_to_card(priv, MVMS_CMD, (void *)cmd, sizeof(*cmd));
	if (ret)
		lbs_pr_alert("SEND_WAKEC_CMD: Host to Card failed for Confirm Wake\n");

	lbs_deb_leave_args(LBS_DEB_HOST, "ret %d", ret);
	return ret;
}

int lbs_process_event(struct lbs_private *priv)
{
	int ret = 0;
	u32 eventcause;

	lbs_deb_enter(LBS_DEB_CMD);

	spin_lock_irq(&priv->driver_lock);
	eventcause = priv->eventcause >> SBI_EVENT_CAUSE_SHIFT;
	spin_unlock_irq(&priv->driver_lock);

	lbs_deb_cmd("event cause %d\n", eventcause);

	switch (eventcause) {
	case MACREG_INT_CODE_LINK_SENSED:
		lbs_deb_cmd("EVENT: MACREG_INT_CODE_LINK_SENSED\n");
		break;

	case MACREG_INT_CODE_DEAUTHENTICATED:
		lbs_deb_cmd("EVENT: deauthenticated\n");
		lbs_mac_event_disconnected(priv);
		break;

	case MACREG_INT_CODE_DISASSOCIATED:
		lbs_deb_cmd("EVENT: disassociated\n");
		lbs_mac_event_disconnected(priv);
		break;

	case MACREG_INT_CODE_LINK_LOST_NO_SCAN:
		lbs_deb_cmd("EVENT: link lost\n");
		lbs_mac_event_disconnected(priv);
		break;

	case MACREG_INT_CODE_PS_SLEEP:
		lbs_deb_cmd("EVENT: sleep\n");

		/* handle unexpected PS SLEEP event */
		if (priv->psstate == PS_STATE_FULL_POWER) {
			lbs_deb_cmd(
			       "EVENT: in FULL POWER mode, ignoreing PS_SLEEP\n");
			break;
		}
		priv->psstate = PS_STATE_PRE_SLEEP;

		lbs_ps_confirm_sleep(priv, (u16) priv->psmode);

		break;

	case MACREG_INT_CODE_HOST_AWAKE:
		lbs_deb_cmd("EVENT: HOST_AWAKE\n");
		lbs_send_confirmwake(priv);
		break;

	case MACREG_INT_CODE_PS_AWAKE:
		lbs_deb_cmd("EVENT: awake\n");
		/* handle unexpected PS AWAKE event */
		if (priv->psstate == PS_STATE_FULL_POWER) {
			lbs_deb_cmd(
			       "EVENT: In FULL POWER mode - ignore PS AWAKE\n");
			break;
		}

		priv->psstate = PS_STATE_AWAKE;

		if (priv->needtowakeup) {
			/*
			 * wait for the command processing to finish
			 * before resuming sending
			 * priv->needtowakeup will be set to FALSE
			 * in lbs_ps_wakeup()
			 */
			lbs_deb_cmd("waking up ...\n");
			lbs_ps_wakeup(priv, 0);
		}
		break;

	case MACREG_INT_CODE_MIC_ERR_UNICAST:
		lbs_deb_cmd("EVENT: UNICAST MIC ERROR\n");
		handle_mic_failureevent(priv, MACREG_INT_CODE_MIC_ERR_UNICAST);
		break;

	case MACREG_INT_CODE_MIC_ERR_MULTICAST:
		lbs_deb_cmd("EVENT: MULTICAST MIC ERROR\n");
		handle_mic_failureevent(priv, MACREG_INT_CODE_MIC_ERR_MULTICAST);
		break;
	case MACREG_INT_CODE_MIB_CHANGED:
	case MACREG_INT_CODE_INIT_DONE:
		break;

	case MACREG_INT_CODE_ADHOC_BCN_LOST:
		lbs_deb_cmd("EVENT: ADHOC beacon lost\n");
		break;

	case MACREG_INT_CODE_RSSI_LOW:
		lbs_pr_alert("EVENT: rssi low\n");
		break;
	case MACREG_INT_CODE_SNR_LOW:
		lbs_pr_alert("EVENT: snr low\n");
		break;
	case MACREG_INT_CODE_MAX_FAIL:
		lbs_pr_alert("EVENT: max fail\n");
		break;
	case MACREG_INT_CODE_RSSI_HIGH:
		lbs_pr_alert("EVENT: rssi high\n");
		break;
	case MACREG_INT_CODE_SNR_HIGH:
		lbs_pr_alert("EVENT: snr high\n");
		break;

	case MACREG_INT_CODE_MESH_AUTO_STARTED:
		/* Ignore spurious autostart events if autostart is disabled */
		if (!priv->mesh_autostart_enabled) {
			lbs_pr_info("EVENT: MESH_AUTO_STARTED (ignoring)\n");
			break;
		}
		lbs_pr_info("EVENT: MESH_AUTO_STARTED\n");
		priv->mesh_connect_status = LBS_CONNECTED;
		if (priv->mesh_open) {
			netif_carrier_on(priv->mesh_dev);
			if (!priv->tx_pending_len)
				netif_wake_queue(priv->mesh_dev);
		}
		priv->mode = IW_MODE_ADHOC;
		schedule_work(&priv->sync_channel);
		break;

	default:
		lbs_pr_alert("EVENT: unknown event id %d\n", eventcause);
		break;
	}

	spin_lock_irq(&priv->driver_lock);
	priv->eventcause = 0;
	spin_unlock_irq(&priv->driver_lock);

	lbs_deb_leave_args(LBS_DEB_CMD, "ret %d", ret);
	return ret;
}
