/* This software is distributed under the following license:
 * http://sflow.net/license.html
 */

#if defined(__cplusplus)
extern "C" {
#endif

#include "hsflowd.h"

#include "regex.h"
#define HSP_DEFAULT_SWITCHPORT_REGEX "^e[0-9]+-[0-9]+-[0-9]+$"
#define HSP_DEFAULT_OS10_PORT 20001

#define HSP_READPACKET_BATCH_OS10 10000
#define HSP_MAX_OS10_MSG_BYTES 10000
#define HSP_OS10_RCV_BUF 8000000

#define HSP_OS10_MIN_POLLING_INTERVAL 10

// HSP_OS10_SWITCHPORT_CONFIG_PROG is defined in hsflowd.h
// because it is used to auto-detect when we are on os10.

#define HSP_OS10_SWITCHPORT_SPEED_PROG "/opt/dell/os10/bin/os10-ethtool"

#define HSP_OS10_SWITCHPORT_STATS_PROG_0 "/opt/dell/os10/bin/os10-show-stats"
#define HSP_OS10_SWITCHPORT_STATS_PROG_1 "ifstat"

  typedef struct _HSP_mod_OS10 {
    // active on two threads (buses)
    EVBus *packetBus;
    EVBus *pollBus;
    // config
    bool configured_socket:1;
    // sample processing
    uint32_t os10_seqno;
    uint32_t os10_drops;
    // counter polling
    SFLAdaptor *poll_current;
    SFLHost_nio_counters ctrs;
    HSP_ethtool_counters et_ctrs;
  } HSP_mod_OS10;

  /*_________________---------------------------__________________
    _________________      readPackets          __________________
    -----------------___________________________------------------
  */

  static void readPackets_os10(EVMod *mod, EVSocket *sock, void *magic)
  {
    HSP_mod_OS10 *mdata = (HSP_mod_OS10 *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);

    int batch = 0;

    if(sp->sFlowSettings == NULL) {
      // config was turned off
      return;
    }

    for( ; batch < HSP_READPACKET_BATCH_OS10; batch++) {
      uint32_t buf32[HSP_MAX_OS10_MSG_BYTES >> 2];
      int recvBytes = recvfrom(sock->fd, (char *)buf32, HSP_MAX_OS10_MSG_BYTES, 0, NULL, NULL);
      if(recvBytes <= 0)
	break;

      myDebug(2, "got OS10 msg: %u bytes", recvBytes);

      if(getDebug() > 2) {
	u_char pbuf[2000];
	printHex((u_char *)buf32, HSP_MAX_OS10_MSG_BYTES, pbuf, 2000, NO);
	myDebug(1, "got msg: %s", pbuf);
      }

     // check metadata signature
      if(buf32[0] != 0xDEADBEEF) {
	myDebug(1, "bad meta-data signature: %08X", buf32[0]);
	continue;
      }

      // meta-data length comes next
      int mdBytes = buf32[1];
      int32_t mdQuads = mdBytes >> 2;

      // tag and len are both 64-bits.  If that changes,  just
      // change the types uses here...
      uint64_t tag;
      uint64_t len;
      int32_t tlQuads = (sizeof(tag) + sizeof(len)) >> 2;

      // values we are looking for:
      uint32_t ifIn=0;
      uint32_t ifOut=0;
      uint32_t seqNo=0;
      uint32_t packetLen=0;

      uint32_t ii = 2;
      for(; ii <= (mdQuads - tlQuads); ) {
	uint32_t val32=0;
	uint64_t val64=0;

	// read tag and length (native byte order)
	memcpy(&tag, &buf32[ii], sizeof(tag));
	ii += sizeof(tag) >> 2;
	memcpy(&len, &buf32[ii], sizeof(len));
	ii += sizeof(len) >> 2;

	// read value
	if(len == 4) val32 = buf32[ii++];
	else if(len == 8) {
	  // collapse val64 to val32
	  memcpy(&val64, &buf32[ii], 8);
	  ii += 2;
	  val32 = (uint32_t)val64;
	}

	switch(tag) {
	case 0: ifIn = val32; break;
	case 1: ifOut = val32; break;
	case 2: seqNo = val32; break;
	case 3: packetLen = val32; break;
	}
      }

      if(ii != mdQuads) {
	myDebug(1, "metadata consumption error");
	continue;
      }

      u_char *pkt = (u_char *)&buf32[mdQuads];
      int headerLen = recvBytes - mdBytes;
      if(headerLen < 14) {
	myDebug(1, "packet too small");
	continue;
      }

      // check for drops indicated by sequence no
      uint32_t droppedSamples = 0;
      if(mdata->os10_seqno) {
	droppedSamples = seqNo - mdata->os10_seqno - 1;
	if(droppedSamples) {
	  mdata->os10_drops += droppedSamples;
	}
      }
      mdata->os10_seqno = seqNo;

      SFLAdaptor *dev_in = NULL;
      SFLAdaptor *dev_out = NULL;

      if(ifIn)
	dev_in = adaptorByIndex(sp, ifIn);
      if(ifOut)
	dev_out = adaptorByIndex(sp, ifOut);

      if(dev_in == NULL
	 || ADAPTOR_NIO(dev_in)->sampling_n_set == 0) {
	// sampling not configured yet - may have just
	// restarted hsflowd
	continue;
      }

      // looks like we get the FCS bytes too -- if the
      // packet is short enough to include them
      int chopped = packetLen - headerLen;
      int fcsBytes = (chopped < 4) ?  4 - chopped : 0;

      takeSample(sp,
		 dev_in,
		 dev_out,
		 NULL, // tap
		 YES, // bridge
		 0, // hook
		 pkt,
		 14,
		 pkt + 14,
		 headerLen - 14 - fcsBytes, /* length of captured payload */
		 packetLen - 14 - 4, /* length of packet (pdu) */
		 droppedSamples,
		 sp->sFlowSettings->samplingRate);
    }
  }

  /*_________________---------------------------__________________
    _________________     openOS10              __________________
    -----------------___________________________------------------
  */

  static int openOS10(EVMod *mod)
  {
    HSP *sp = (HSP *)EVROOTDATA(mod);

    // register call-backs
    uint16_t os10Port = sp->os10.port ?: HSP_DEFAULT_OS10_PORT;
    int fd = 0;
    if(os10Port) {
      // TODO: should this really be "::1" and PF_INET6?  Or should we bind to both "127.0.0.1" and "::1" (cf mod_json)
      fd = UTSocketUDP("127.0.0.1", PF_INET, os10Port, HSP_OS10_RCV_BUF);
     myDebug(1, "os10 socket is %d", fd);
    }

    return fd;
  }

  /*_________________---------------------------__________________
    _________________     setSamplingRate       __________________
    -----------------___________________________------------------
  */

  static int srateOutputLine(void *magic, char *line) {
    return YES;
  }

  static bool setSamplingRate(EVMod *mod, SFLAdaptor *adaptor) {
    HSPAdaptorNIO *niostate = ADAPTOR_NIO(adaptor);

    if(adaptor->ifSpeed == 0) {
      // by refusing to set a sampling rate for a port
      // with speed == 0 we can stabilize the startup.
      // Now sampling will only be configured as ports
      // are discovered or come up (or change speed).
      // TODO: if a port has gone down do we need to
      // clear the sampling rate back to 0, since it
      // may come up again with a different speed?
      return NO;
    }

    if(niostate->switchPort == NO
       || niostate->loopback
       || niostate->bond_master) {
      return NO;
    }
    HSP *sp = (HSP *)EVROOTDATA(mod);
    HSPSFlowSettings *settings = sp->sFlowSettings;

    int hw_sampling = YES;
    UTStringArray *cmdline = strArrayNew();
    strArrayAdd(cmdline, HSP_OS10_SWITCHPORT_CONFIG_PROG);
    // usage:  <prog> [enable|disable] <interface> <direction>  <rate>
#define HSP_MAX_TOK_LEN 16
    strArrayAdd(cmdline, "enable");
    strArrayAdd(cmdline, adaptor->deviceName);
    strArrayAdd(cmdline, "ingress");
    strArrayAdd(cmdline, "0");  // placeholder for sampling N in slot 4
    strArrayAdd(cmdline, NULL); // extra NULL
#define HSP_MAX_EXEC_LINELEN 1024
    char outputLine[HSP_MAX_EXEC_LINELEN];
    niostate->sampling_n = lookupPacketSamplingRate(adaptor, settings);
    if(niostate->sampling_n != niostate->sampling_n_set) {
      myDebug(1, "setSwitchPortSamplingRate(%s) %u -> %u",
	      adaptor->deviceName,
	      niostate->sampling_n_set,
	      niostate->sampling_n);
      char srate[HSP_MAX_TOK_LEN];
      snprintf(srate, HSP_MAX_TOK_LEN, "%u", niostate->sampling_n);
      strArrayInsert(cmdline, 4, srate);

      if(debug(1)) {
	char *cmd_str = strArrayStr(cmdline, NULL, NULL, " ", NULL);
	myDebug(1, "exec command:[%s]", cmd_str);
	my_free(cmd_str);
      }

      int status;
      if(myExec(niostate, strArray(cmdline), srateOutputLine, outputLine, HSP_MAX_EXEC_LINELEN, &status)) {
	if(WEXITSTATUS(status) != 0) {
	  myLog(LOG_ERR, "myExec(%s) exitStatus=%d so assuming ULOG/NFLOG is 1:1",
		HSP_OS10_SWITCHPORT_CONFIG_PROG,
		WEXITSTATUS(status));
	  hw_sampling = NO;
	}
	else {
	  myDebug(1, "setSwitchPortSamplingRate(%s) succeeded", adaptor->deviceName);
	  // hardware or kernel sampling was successfully configured
	  niostate->sampling_n_set = niostate->sampling_n;
	  sp->hardwareSampling = YES;
	}
      }
      else {
	myLog(LOG_ERR, "myExec() calling %s failed (adaptor=%s)",
	      strArrayAt(cmdline, 0),
	      adaptor->deviceName);
      }
    }
    strArrayFree(cmdline);
    return hw_sampling;
  }

  /*_________________---------------------------__________________
    _________________    pollCounters           __________________
    -----------------___________________________------------------
  */

  UTStringArray *tokenize(char *in, char *sep, char *buf, int bufLen) {
    UTStringArray *ans = strArrayNew();
    char *p = in;
    while(parseNextTok(&p, sep, NO, 0, YES, buf, bufLen) != NULL)
      if(my_strlen(buf)) strArrayAdd(ans, buf);
    return ans;
  }

#define OS10_DELLIF "dell-if/"
#define OS10_IFSTATE "if/interfaces-state/interface/statistics"

  static void setPollCurrent(EVMod *mod, SFLAdaptor *adaptor)
  {
    HSP_mod_OS10 *mdata = (HSP_mod_OS10 *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    if(mdata->poll_current != adaptor) {
      // TODO: accumulate counters if changing from adaptor to adaptor (or NULL)
      if(mdata->poll_current) {
	accumulateNioCounters(sp, mdata->poll_current, &mdata->ctrs, &mdata->et_ctrs);
	ADAPTOR_NIO(mdata->poll_current)->last_update = sp->pollBus->clk;
      }
      mdata->poll_current = adaptor;
      memset(&mdata->ctrs, 0, sizeof(mdata->ctrs));
      memset(&mdata->et_ctrs, 0, sizeof(mdata->et_ctrs));
    }
  }

  static int pollAllOutputLine(void *magic, char *line) {
    EVMod *mod = (EVMod *)magic;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    HSP_mod_OS10 *mdata = (HSP_mod_OS10 *)mod->data;
    char tokbuf[HSP_MAX_EXEC_LINELEN];
    UTStringArray *tokens = tokenize(line, " =", tokbuf, HSP_MAX_EXEC_LINELEN);
    char *tok0 = strArrayAt(tokens, 0);
    char *tok1 = strArrayAt(tokens, 1);
    SFLMacAddress mac;
    if(my_strequal(tok0, OS10_DELLIF OS10_IFSTATE "phys-address")) {
      if(hexToBinary((u_char *)tok1, (u_char *)&mac.mac, 6) == 6) {
	setPollCurrent(mod, adaptorByMac(sp, &mac));
      }
    }

    if(mdata->poll_current == NULL)
      return YES;

    SFLAdaptor *adaptor = mdata->poll_current;
    //HSPAdaptorNIO *nio = ADAPTOR_NIO(adaptor);

    if(my_strequal(tok0, OS10_IFSTATE "speed")) {
      setAdaptorSpeed(sp, adaptor, strtoll(tok1, NULL, 0));
    }

    if(my_strequal(tok0, OS10_IFSTATE "if-index")) {
      uint32_t ifIndex = strtol(tok1, NULL, 0);
      if(ifIndex != adaptor->ifIndex) {
	myLog(LOG_ERR, "ifIndex mismatch for interface %s: %u", adaptor->deviceName, ifIndex);
      }
    }

    if(my_strequal(tok0, OS10_IFSTATE "name")) {
      if(tok1 && my_strequal(tok1, adaptor->deviceName) == NO) {
	myLog(LOG_ERR, "name mismatch for interface %s != %s", tok1, adaptor->deviceName);
      }
    }

    // TODO: handle these properly
    if(my_strequal(tok0, OS10_IFSTATE "admin-status")) { }
    if(my_strequal(tok0, OS10_IFSTATE "oper-status")) { }
    if(my_strequal(tok0, OS10_IFSTATE "enabled")) { }
    
    if(my_strequal(tok0, OS10_IFSTATE "in-octets")) mdata->ctrs.bytes_in = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "out-octets")) mdata->ctrs.bytes_out = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "in-unicast-pkts")) mdata->ctrs.pkts_in = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "out-unicast-pkts")) mdata->ctrs.pkts_out = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "in-errors")) mdata->ctrs.errs_in = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "out-errors")) mdata->ctrs.errs_out = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "in-discards")) mdata->ctrs.drops_in = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "out-discards")) mdata->ctrs.drops_out = strtoll(tok1, NULL, 0);
    
    if(my_strequal(tok0, OS10_IFSTATE "in-unknown-protos")) mdata->et_ctrs.unknown_in = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "in-multicast-pkts")) mdata->et_ctrs.mcasts_in = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "out-multicast-pkts")) mdata->et_ctrs.mcasts_out = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "in-broadcast-pkts")) mdata->et_ctrs.bcasts_in = strtoll(tok1, NULL, 0);
    if(my_strequal(tok0, OS10_IFSTATE "out-broadcast-pkts")) mdata->et_ctrs.bcasts_out = strtoll(tok1, NULL, 0);

    strArrayFree(tokens);
    return YES;
  }

  static bool pollAllCounters(EVMod *mod) {
    UTStringArray *cmdline = strArrayNew();
    strArrayAdd(cmdline, HSP_OS10_SWITCHPORT_STATS_PROG_0);
    strArrayAdd(cmdline, HSP_OS10_SWITCHPORT_STATS_PROG_1);
    strArrayAdd(cmdline, NULL); // trailing NULL

#define HSP_MAX_EXEC_LINELEN 1024
    char outputLine[HSP_MAX_EXEC_LINELEN];

    if(debug(1)) {
      char *cmd_str = strArrayStr(cmdline, NULL, NULL, " ", NULL);
      myDebug(1, "exec command:[%s]", cmd_str);
      my_free(cmd_str);
    }

    int status;
    if(myExec(mod, strArray(cmdline), pollAllOutputLine, outputLine, HSP_MAX_EXEC_LINELEN, &status)) {
      if(WEXITSTATUS(status) != 0) {
	myLog(LOG_ERR, "myExec(%s) exitStatus=%d",
	      HSP_OS10_SWITCHPORT_SPEED_PROG,
	      WEXITSTATUS(status));
      }
      else {
	myDebug(1, "pollAllCounters() succeeded");
      }
    }
    else {
      myLog(LOG_ERR, "myExec() calling %s failed",
	    strArrayAt(cmdline, 0));
    }
    setPollCurrent(mod, NULL);
    strArrayFree(cmdline);
    return YES;
  }

 /*_________________---------------------------__________________
    _________________   markSwitchPort         __________________
    -----------------__________________________------------------
  */

  static bool markSwitchPort(EVMod *mod, SFLAdaptor *adaptor)  {
    HSP *sp = (HSP *)EVROOTDATA(mod);

    if(sp->os10.swp_regex_str == NULL) {
      // pattern not specified in config, so compile the default
      sp->os10.swp_regex_str = HSP_DEFAULT_SWITCHPORT_REGEX;
      sp->os10.swp_regex = UTRegexCompile(HSP_DEFAULT_SWITCHPORT_REGEX);
      assert(sp->os10.swp_regex);
    }

    // use pattern to mark the switch ports
    bool switchPort = NO;
    if(regexec(sp->os10.swp_regex, adaptor->deviceName, 0, NULL, 0) == 0) {
      switchPort = YES;
    }
    HSPAdaptorNIO *niostate = ADAPTOR_NIO(adaptor);
    niostate->switchPort = switchPort;
    return switchPort;
  }

  /*_________________---------------------------__________________
    _________________    evt_config_changed     __________________
    -----------------___________________________------------------
  */

  static void evt_pkt_config_changed(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_OS10 *mdata = (HSP_mod_OS10 *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);

    if(sp->sFlowSettings == NULL)
      return; // no config (yet - may be waiting for DNS-SD)

    if(!mdata->configured_socket) {
      int fd = openOS10(mod);
      if(fd > 0)
	EVBusAddSocket(mod, mdata->packetBus, fd, readPackets_os10, mod);
      mdata->configured_socket = YES;
    }
  }

  static void evt_poll_config_changed(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP *sp = (HSP *)EVROOTDATA(mod);

    if(sp->sFlowSettings == NULL)
      return; // no config (yet - may be waiting for DNS-SD)

    // The sampling-rate settings may have changed.
    SFLAdaptor *adaptor;
    UTHASH_WALK(sp->adaptorsByName, adaptor) {
	setSamplingRate(mod, adaptor);
      }
  }

  /*_________________---------------------------__________________
    _________________      evt_intf_read        __________________
    -----------------___________________________------------------
  */

  static void evt_poll_intf_read(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    SFLAdaptor *adaptor = *(SFLAdaptor **)data;
    if(markSwitchPort(mod, adaptor)) {
      HSPAdaptorNIO *nio = ADAPTOR_NIO(adaptor);
      // turn off the use of ethtool_GSET so it doesn't get the wrong speed
      nio->ethtool_GSET = NO;
      // TODO: possibly turn off these as well
      // nio->ethtool_GDRVINFO = NO;
      // nio->ethtool_GLINKSETTINGS = NO;
      // nio->ethtool_GSTATS = NO;
    }
  }

  /*_________________---------------------------__________________
    _________________      evt_intfs_changed    __________________
    -----------------___________________________------------------
  */

  static void evt_poll_intfs_changed(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    // need to refresh speed/status meta-data for all interfaces
    // may trigger sampling-rate setting if speed changes (see below)
    pollAllCounters(mod);
  }

  /*_________________---------------------------__________________
    _________________   evt_poll_speed_changed  __________________
    -----------------___________________________------------------
  */

  static void evt_poll_speed_changed(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    SFLAdaptor *adaptor = *(SFLAdaptor **)data;

    HSP *sp = (HSP *)EVROOTDATA(mod);
    if(sp->sFlowSettings == NULL)
      return; // no config (yet - may be waiting for DNS-SD)

    setSamplingRate(mod, adaptor);
  }

  /*_________________---------------------------__________________
    _________________     evt_poll_update_nio   __________________
    -----------------___________________________------------------
  */

  static void evt_poll_update_nio(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP *sp = (HSP *)EVROOTDATA(mod);

    if(sp->sFlowSettings == NULL)
      return; // no config (yet - may be waiting for DNS-SD)
    
    // always update all counters
    pollAllCounters(mod);
    sp->nio_last_update = sp->pollBus->clk;
  }

  /*_________________---------------------------__________________
    _________________    module init            __________________
    -----------------___________________________------------------
  */

  void mod_os10(EVMod *mod) {
    HSP *sp = (HSP *)EVROOTDATA(mod);
    mod->data = my_calloc(sizeof(HSP_mod_OS10));
    HSP_mod_OS10 *mdata = (HSP_mod_OS10 *)mod->data;
    mdata->packetBus = EVGetBus(mod, HSPBUS_PACKET, YES);
    mdata->pollBus = EVGetBus(mod, HSPBUS_POLL, YES);

    EVEventRx(mod, EVGetEvent(mdata->pollBus, HSPEVENT_INTF_READ), evt_poll_intf_read);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, HSPEVENT_INTFS_CHANGED), evt_poll_intfs_changed);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, HSPEVENT_INTF_SPEED), evt_poll_speed_changed);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, HSPEVENT_UPDATE_NIO), evt_poll_update_nio);

    EVEventRx(mod, EVGetEvent(mdata->pollBus, HSPEVENT_CONFIG_CHANGED), evt_poll_config_changed);
    EVEventRx(mod, EVGetEvent(mdata->packetBus, HSPEVENT_CONFIG_CHANGED), evt_pkt_config_changed);

    // we know there are no 32-bit counters
    sp->nio_polling_secs = 0;

    // set a minimum polling interval
    if(sp->minPollingInterval < HSP_OS10_MIN_POLLING_INTERVAL) {
      sp->minPollingInterval = HSP_OS10_MIN_POLLING_INTERVAL;
    }
    // ask for polling to be sync'd so that clusters of interfaces are polled together.
    if(sp->syncPollingInterval < HSP_OS10_MIN_POLLING_INTERVAL) {
      sp->syncPollingInterval = HSP_OS10_MIN_POLLING_INTERVAL;
    }
  }

#if defined(__cplusplus)
} /* extern "C" */
#endif