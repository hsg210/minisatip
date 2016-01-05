/*
 * Copyright (C) 2014-2020 Catalin Toda <catalinii@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 *
 */

#include "netceiver.h"

extern struct struct_opts opts;

SNetceiver sn[MAX_ADAPTERS];
#define SN sn[ad->id]

int handle_ts (unsigned char *buffer, size_t len, void *p);
int handle_ten (tra_t *ten, void *p);

extern char *fe_pilot[];
extern char *fe_rolloff[];
extern char *fe_delsys[];
extern char *fe_fec[];
extern char *fe_modulation[];
extern char *fe_tmode[];
extern char *fe_gi[];
extern char *fe_hierarchy[];
extern char *fe_specinv[];
extern char *fe_pol[];


int netcv_close(adapter *ad)
{
	SN.want_commit = 0;

	if (!SN.ncv_rec) LOGL(0, "netceiver: receiver instance is NULL (id=%d)", ad->id);

	LOGL(0, "netceiver: delete receiver instance for adapter %d", ad->id);

	/* unregister handlers */
	register_ten_handler(SN.ncv_rec, NULL, NULL);
	register_ts_handler(SN.ncv_rec, NULL, NULL);

	/* call recv_init of libmcli to clean up the NetCeiver API */
	recv_del (SN.ncv_rec);
	SN.ncv_rec = NULL;

	ad->strength = 0;
	ad->status = 0;
	ad->snr = 0;
	ad->ber = 0;

	return 0;
}


int netcv_open_device(adapter *ad)
{
	fprintf(stderr, "REEL: netcv_open_device (id=%d)\n", ad->id);

	SN.want_commit = 0;
	SN.ncv_rec = NULL;

	return 0;
}


int netcv_set_pid(adapter *ad, uint16_t pid)
{
	//fprintf(stderr, "REEL: netcv_set_pid (id=%d, pid=%d)\n", ad->id, pid);

	int aid = ad->id;
	LOG("netceiver: set_pid for adapter %d, pid %d, err %d", aid, pid, SN.err);

	if (SN.err) // error reported, return error
		return 0;

	SN.npid[SN.lp] = pid;
	SN.lp++;
	SN.want_commit = 1;

	return aid + 100;
}


int netcv_del_pid(int fd, int pid)
{
	//fprintf(stderr, "REEL: netcv_del_pid (id=%d, pid=%d)\n", fd, pid);

	int i, hit = 0;
	adapter *ad;
	fd -= 100;
	ad = get_adapter(fd);
	if (!ad)
		return 0;
	LOG("netceiver: del_pid for aid %d, pid %d, err %d", fd, pid, SN.err);
	if (SN.err) // error reported, return error
		return 0;

	for(i = 0; i < MAX_PIDS; i++)
		if (SN.npid[i] == pid || hit)
		{
			SN.npid[i] = SN.npid[i + 1];
			hit = 1;
		}
	if (hit && sn[fd].lp > 0) sn[fd].lp--;
	SN.want_commit = 1;

	return 0;
}


void netcv_commit(adapter *ad)
{
	fprintf(stderr, "REEL: netcv_commit (id=%d, want_tune=%d, want_commit=%d, lp=%d)\n",
			ad->id, SN.want_tune, SN.want_commit, SN.lp);

	int i;

	int m_pos = 0;
	fe_type_t type;
	recv_sec_t m_sec;
	struct dvb_frontend_parameters m_fep;
	dvb_pid_t m_pids[MAX_PIDS];

	/* check if we have to create a receiver instance first */
	SN.err = 0;
	if (!SN.ncv_rec)
	{
		LOGL(0, "netceiver: add a new receiver instance for adapter %d", ad->id);

		/* call recv_add of libmcli to add a new receiver instance */
		SN.ncv_rec = recv_add();

		/* register handlers */
		register_ten_handler(SN.ncv_rec, &handle_ten, ad);
		register_ts_handler(SN.ncv_rec, &handle_ts, sn + ad->id);

		if (!SN.ncv_rec) SN.err = 1;

		SN.want_tune = 0; // wait until netcv_tune triggers the tuning
	}

	/* tune receiver to a new frequency / tranponder */
	if (SN.want_tune)
	{
		transponder *tp = &ad->tp;
		LOGL(0, "tuning to %d@%d pol: %s (%d) sr:%d fec:%s delsys:%s mod:%s (rolloff:%s) (pilot:%s)",
				tp->freq, tp->diseqc, get_pol(tp->pol), tp->pol, tp->sr,
				fe_fec[tp->fec], fe_delsys[tp->sys], fe_modulation[tp->mtype],
				fe_rolloff[tp->ro], fe_pilot[tp->plts]);

		int map_pos[] = { 0, 192, 130, 282, -50 };
		int map_pol[] = { 0, SEC_VOLTAGE_13, SEC_VOLTAGE_18, SEC_VOLTAGE_OFF };

		switch (tp->sys)
		{
			case SYS_DVBS:
			case SYS_DVBS2:
				m_pos = 1800 + map_pos[tp->diseqc];

				memset(&m_sec, 0, sizeof(recv_sec_t));
				m_sec.voltage = map_pol[tp->pol];

				memset(&m_fep, 0, sizeof(struct dvb_frontend_parameters));
				m_fep.frequency = tp->freq;
				m_fep.inversion = INVERSION_AUTO;
				m_fep.u.qpsk.symbol_rate = tp->sr;

				if (tp->sys == SYS_DVBS)
				{
					m_fep.u.qpsk.fec_inner = tp->fec;
					type = FE_QPSK;
				}
				else
				{
					// Für DVB-S2 PSK8 oder QPSK, siehe vdr-mcli-plugin/device.c
					if (tp->mtype)	m_fep.u.qpsk.fec_inner = tp->fec | (PSK8 << 16);
					else		m_fep.u.qpsk.fec_inner = tp->fec | (QPSK_S2 << 16);
					type = FE_DVBS2;
				}
				break;

				/* set roll-off */
				// TODO: check if needed for DVB-S2 transponders
				m_fep.u.qpsk.fec_inner |= (tp->ro << 24);


				break;

			case SYS_DVBC_ANNEX_A:
				m_pos = 0xfff; /* not sure, to be tested */
				memset(&m_sec, 0, sizeof(recv_sec_t));

				m_fep.frequency = tp->freq;
				m_fep.inversion = INVERSION_AUTO;
				m_fep.u.qam.fec_inner = FEC_NONE;
				m_fep.u.qam.modulation = tp->mtype;
				m_fep.u.qam.symbol_rate = tp->sr;

				break;

			case SYS_DVBT:
				break;
		}

		memset(m_pids, 0, sizeof(m_pids));
		m_pids[0].pid=-1;

		/* call recv_tune of libmcli */
		if(recv_tune(SN.ncv_rec, type, m_pos, &m_sec, &m_fep, m_pids) != 0)
			LOGL(0, "netceiver: Tuning receiver failed");

		ad->strength = 0;
		ad->status = 0;
		ad->snr = 0;
		ad->ber = 0;

		SN.want_tune = 0;
	}

	/* activate or deactivate PIDs */
	if (SN.want_commit)
	{
		if (SN.lp)
		{
			memset(m_pids, 0, sizeof(m_pids));
			for(i = 0; i < SN.lp; i++)
			{
				m_pids[i].pid = SN.npid[i];
				m_pids[i].id = 0; // here we maybe have to set the SID if this PID is encrypted

			}

			m_pids[i].pid = -1;
			/* call recv_pids of libmcli to set the active PIDs */
			if(recv_pids (SN.ncv_rec, m_pids))
				LOGL(0, "netceiver: seting PIDs failed");
		}
		else
		{
			/* call recv_stop of libmcli to deactivate all PIDs */
			if(recv_stop (SN.ncv_rec))
				LOGL(0, "netceiver: removing all PIDs failed");
		}

		SN.want_commit = 0;
	}

	return;
}


int netcv_tune(int aid, transponder * tp)
{
	fprintf(stderr, "REEL: netcv_tune (id=%d, tp.freq=%d)\n", aid, tp->freq);

	adapter *ad = get_adapter(aid);
	if (!ad)
		return 1;

	if (tp->sys == SYS_DVBT)
		LOGL(0, "netceiver: Sorry, DVB-T not yet implemented");

	SN.want_tune = 1;	// we do not tune right now, just set the flag for netcv_commit
	SN.want_commit = 0;
	SN.lp = 0;		// make sure number of active pids is 0 after tuning
	return 0;
}


fe_delivery_system_t netcv_delsys(int aid, int fd, fe_delivery_system_t *sys)
{
	//fprintf(stderr, "REEL: netcv_delsys (id=%d)\n", aid);
	return 0;
}


void find_netcv_adapter(adapter **a)
{
	int i, k, n, na;
	netceiver_info_list_t *nc_list;
	adapter *ad;

	// find 1st free adapter
	for (na = 0; na < MAX_ADAPTERS; na++)
		if (!a[na] || (a[na]->pa == -1 && a[na]->fn == -1)) break;

	/* call recv_init of libmcli to initialize the NetCeiver API */
	if(recv_init("vlan4", 23000))
		LOGL(0, "Netceiver init failed");
 
	fprintf(stderr, "REEL: Search for %d Netceivers... ", opts.netcv_count);
	n = 0;
	do
	{
		usleep(500000);
		fprintf(stderr, "####");
		nc_list = nc_get_list();
	} while (nc_list->nci_num < opts.netcv_count && n++ < 20);
	nc_lock_list ();
	fprintf(stderr, "\n");

	// loop trough list of found netceivers and ad them to list of adapters
	for (n = 0; n < nc_list->nci_num; n++) {
		netceiver_info_t *nci = nc_list->nci + n;
		fprintf (stderr, "Found NetCeiver: %s \n", nci->uuid);
			for (i = 0; i < nci->tuner_num; i++) {
				// TODO: implement disable (low prio)

				if (na >= MAX_ADAPTERS) break;
				if (!a[na]) a[na] = malloc1(sizeof(adapter));

				fprintf (stderr, "  Tuner: %s, Type %d\n", nci->tuner[i].fe_info.name, nci->tuner[i].fe_info.type);

				ad = a[na];
				ad->pa = 0;
				ad->fn = 0;
				sn[na].want_tune = 0;
				sn[na].want_commit = 0;
				sn[na].ncv_rec = NULL;

				/* initialize signal status info */
				ad->strength = 0;
				ad->max_strength = 0xff;
				ad->status = 0;
				ad->snr = 0;
				ad->max_snr = 0xff;
				ad->ber = 0;

				/* register callback functions in adapter structure */
				ad->open = (Open_device) netcv_open_device;
				ad->set_pid = (Set_pid) netcv_set_pid;
				ad->del_filters = (Del_filters) netcv_del_pid;
				ad->commit = (Adapter_commit) netcv_commit;
				ad->tune = (Tune) netcv_tune;
				ad->delsys = (Dvb_delsys) netcv_delsys;
				ad->post_init = (Adapter_commit) NULL;
				ad->close = (Adapter_commit) netcv_close;

				/* register delivery system type */
				for (k = 0; k < 10; k++) ad->sys[k] = 0;
				switch(nci->tuner[i].fe_info.type)
				{
				case FE_DVBS2:
					ad->sys[0] = ad->tp.sys = SYS_DVBS2;
					ad->sys[1] = SYS_DVBS;
					break;

				case FE_QPSK:
					ad->sys[0] = ad->tp.sys = SYS_DVBS;
					break;

				case FE_QAM:
					ad->sys[0] = ad->tp.sys = SYS_DVBC_ANNEX_A;
					break;

				case FE_OFDM:
					//ad->sys[0] = SYS_DVBT; // DVB-T not yet implemented...
					break;
				}

				/* create pipe for TS data transfer from libmcli to minisatip */
				int pipe_fd[2];
				if (pipe2 (pipe_fd, O_NONBLOCK)) LOGL (0, "netceiver: creating pipe failed");
				ad->dvr = pipe_fd[0];		// read end of pipe
				sn[na].pwfd = pipe_fd[1];	// write end of pipe

				na++; // increase number of tuner count
			}
	}
	nc_unlock_list(); // netceivers appearing after this will be recognized by libmcli but will not made available to minisatip

	for (; na < MAX_ADAPTERS; na++)
		if (a[na])
			a[na]->pa = a[na]->fn = -1;
}


/*
 * Handle TS data
 * This function is called by libmcli each time a IP packet with TS packets arrives.
 * We write the data to the write end of a pipe
 *
 */

int handle_ts (unsigned char *buffer, size_t len, void *p) {
	//fprintf(stderr, "(%d) ", len);
	SNetceiver *nc = p;
	size_t lw;

	if(nc->lp == 0) return len;

	lw = write(nc->pwfd, buffer, len);
	if (lw != len) LOGL(0, "netceiver: not all data forwarded...");


	return len;

	switch(len) {
		case 1316: // 7 TS packets
			fprintf(stderr, "\bO");
			break;

		case 188: // 1 TS packet
			fprintf(stderr, "\b.");
			break;

		default:
			fprintf(stderr, "\bo");
	}

	return len;
}

/* Handle signal status information */
int handle_ten (tra_t *ten, void *p) {
	adapter *ad = p;
	recv_festatus_t *festat;

	if(ten) {
		festat = &ten->s;
		ad->strength = (festat->strength & 0xffff) >> 8;
		ad->status = festat->st == 0x1f ? FE_HAS_LOCK : 0;
		ad->snr = (festat->snr & 0xffff) >> 8;
		ad->ber = festat->ber;

		return 0;
		fprintf(stderr, "\nStatus: %02X, Strength: %04X, SNR: %04X, BER: %04X -  ",
				festat->st, festat->strength, festat->snr, festat->ber);
	}
	return 0;
}