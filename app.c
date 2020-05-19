/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2015 Intel Corporation
 */

/* typical launch: sudo ./build/app -l 1 -n 4 */

#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <time.h>

#define NUM_MBUFS 		8191
#define RX_RING_SIZE 	1024
#define TX_RING_SIZE 	1024
#define MBUF_CACHE_SIZE 250
#define BURST_SZIE		32

// todo: extract methods
static int port_init(uint16_t port_id, struct rte_mempool* mbuf_pool)
{
	const uint16_t rx_rings = 1,					///> number of receive queues, 
					tx_rings = 1;					///> number of transmit queues,
	
	///> get NUMA socket to which an Ethernet device is connected
	const int numa_socket_id = rte_eth_dev_socket_id(port_id); ///!!! warning !!! not sure it is really const in runtime

	uint16_t nb_rxd = RX_RING_SIZE,			///> size of each RX ring (count of descriptors there)
			nb_txd = TX_RING_SIZE;			///> size of each TX ring (count of descriptors there)

	struct rte_eth_conf port_conf = { 				///> used to configure an Ethernet port
		.rxmode = {									///> RX features of an Ethernet port
			.max_rx_pkt_len = RTE_ETHER_MAX_LEN,	///> only used if packets size > 1540 (JUMBO_FRAME enabled)
		},
	};
	struct rte_eth_txconf txconf;					///> used to configure a TX ring of an Ethernet port
	struct rte_ether_addr addr;						///> contains 6 bytes to keep MAC of a device
	int retval;
	uint16_t rings_iterator;						///> used in for loops to iterate rings
	struct rte_eth_dev_info dev_info;

	if (!rte_eth_dev_is_valid_port(port_id)) 			///> if port_id is ok (device binded -> ok)
		return -6; // ENXIO

//todo: not do it and delete optimization stuff
	retval = rte_eth_dev_info_get(port_id, &dev_info); ///> writes contextual information about Ethernet device
	if (retval) {
		printf("failed get device (port %u) info: %s\n", port_id, strerror(-retval));
		return retval;
	}

/*	Per-port Tx offloads to be set using DEV_TX_OFFLOAD_* flags.
	Only offloads set on tx_offload_capa field on rte_eth_dev_info structure are allowed to be set
*/
	if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;

	///> todo: describe details
	retval = rte_eth_dev_configure(port_id, rx_rings, tx_rings, &port_conf);
	if (retval)
		return retval;

	///> adjusting number of receive descriptor [nb_rxd] and
	///> number of transmit descriptor [nb_txd] (if necessary)
	///> todo: describe details
	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
	if (retval)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (rings_iterator = 0; rings_iterator < rx_rings; ++rings_iterator) {		
		retval = rte_eth_rx_queue_setup(port_id, rings_iterator, nb_rxd,
			numa_socket_id,
			NULL,
			mbuf_pool);
		if (retval < 0)
			return retval;
	}

/*	Per-queue Tx offloads to be set using DEV_TX_OFFLOAD_* flags.
	Only offloads set on tx_queue_offload_capa or tx_offload_capa fields on rte_eth_dev_info structure are allowed to be set.
*/
	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	for (rings_iterator = 0; rings_iterator < tx_rings; ++rings_iterator) {
		retval = rte_eth_tx_queue_setup(port_id, rings_iterator, nb_txd,
			numa_socket_id,	&txconf);

		if (retval < 0)
			return retval;
	}

/* tx and rx queues are ready so we r starting up*/
	retval = rte_eth_dev_start(port_id);
	if (retval < 0)
		return retval;

// display the port MAC address
	retval = rte_eth_macaddr_get(port_id, &addr);
	if (retval)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
		" %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n", port_id,
		addr.addr_bytes[0], addr.addr_bytes[1],
		addr.addr_bytes[2], addr.addr_bytes[3],
		addr.addr_bytes[4], addr.addr_bytes[5]);

/* Enable RX in promiscuous mode for the Ethernet device. */
	retval = rte_eth_promiscuous_enable(port_id);
	if (retval)
		return retval;

	return 0;
}

static __attribute__((noreturn)) void
lcore_main(void)
{
	uint16_t port_id;

/*
 * Check that the port is on the same NUMA node as the polling thread for best performance.
 */
	RTE_ETH_FOREACH_DEV(port_id) {
		const int dev_numa_socket_id = rte_eth_dev_socket_id(port_id);
		const int app_numa_socket_id = (int)(rte_socket_id());

		if (dev_numa_socket_id > 0 && dev_numa_socket_id != app_numa_socket_id)
			printf("WARNING, port %u is on remote NUMA node to polling thread\n"
				"\tPerfomance will not be optimal\n", port_id);
	}

	for (port_id = 0;;) {
		const uint16_t queue_id = 0;
		struct rte_mbuf *packet_buffers[BURST_SZIE];
		uint16_t nb_tx_packets;
		const uint16_t nb_rx_packets = rte_eth_rx_burst(port_id,
			queue_id, packet_buffers, BURST_SZIE);

		if (unlikely(nb_rx_packets == 0))
				continue;

		rte_eth_tx_burst(port_id+1, queue_id, packet_buffers, nb_rx_packets);	///> no ctrl for shortness

		nb_tx_packets = rte_eth_tx_burst(port_id+2, queue_id,
			packet_buffers, nb_rx_packets);

		if(unlikely(nb_tx_packets != nb_rx_packets)) {
			uint16_t iter = nb_tx_packets;
			for (;iter < nb_rx_packets; ++iter)
				rte_pktmbuf_free(packet_buffers[iter]);
		}
	}

}

int main(int argc, char* argv[])
{
	clock_t start = clock(), finish;
	double elapsed;
	uint16_t nb_ports,
			port_id;			///> used to keep IDs of binded devices (available ports)
	struct rte_mempool *mbuf_pool;
	int numa_socket_id;			///>  ID of the physical socket of the logical core we are running on
	int processed_params = rte_eal_init(argc, argv);	
	if (processed_params < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization");

	finish = clock();
	elapsed = ((double)(finish - start)) / CLOCKS_PER_SEC;
	printf("rte_eal_init spent %f seconds\n", elapsed);


	nb_ports = rte_eth_dev_count_avail(); ///> get number of binded port(devices)
	printf("%u binded ports detected\n", nb_ports);
	
	numa_socket_id = rte_socket_id(); ///> eturn the ID of the physical socket of the logical core we are running on
	printf("numa_socket_id: %u\n", numa_socket_id);

/* Creates a new mempool in memory to hold the mbufs. (used for Rx packets)*/
	mbuf_pool = rte_pktmbuf_pool_create(
		"MBUF_POOL",				///> memory pool string id
		NUM_MBUFS * nb_ports,		///> number of elements
		MBUF_CACHE_SIZE,			///> per-core cache to avoid requests to the memory pool’s ring
		0,							/// > size of private data field (user defined) to keep some data to share between workers
		RTE_MBUF_DEFAULT_BUF_SIZE,	///> size of data buffer in each mbuf <???>
		numa_socket_id);			///> above (main::)

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "cannot create mbuf pool\n");


	RTE_ETH_FOREACH_DEV(port_id)
		if (port_init(port_id, mbuf_pool))
			rte_exit(EXIT_FAILURE, "cannot init port %"PRIu16 "\n", port_id);

	lcore_main();
	return 0;
}