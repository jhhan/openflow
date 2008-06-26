#include <linux/etherdevice.h>
#include "compat.h"
#include "hwtable_nf2/hwtable_nf2.h"
#include "crc32.h"

/* For NetFPGA */
#include "hwtable_nf2/nf2.h"
#include "hwtable_nf2/reg_defines.h"
#include "hwtable_nf2/nf2_export.h"

spinlock_t wildcard_free_lock;
struct list_head wildcard_free_list;

spinlock_t exact_free_lock;
struct sw_flow_nf2* exact_free_list[OPENFLOW_NF2_EXACT_TABLE_SIZE];

struct net_device* nf2_get_net_device(void) {
	return dev_get_by_name(&init_net, "nf2c0");	
}

void nf2_free_net_device(struct net_device* dev) {
	dev_put(dev);	
}


/*
 * Checks to see if the actions requested by the flow are capable of being 
 * done in the NF2 hardware. Returns 1 if yes, 0 for no.
 */
int nf2_are_actions_supported(struct sw_flow *flow) {
	int i;
	for (i=0; i < flow->n_actions; ++i) {
		// Currently only support the output port(s) action
		if (flow->actions[i].type != OFPAT_OUTPUT) {
			printk("---Flow type != OFPAT_OUTPUT---\n");
			return 0;
		}
		
		// Only support ports 0-3, ALL, FLOOD. Let CONTROLLER/LOCAL fall through
		if (!(ntohs(flow->actions[i].arg.output.port) < 4) &&
			!(ntohs(flow->actions[i].arg.output.port) == OFPP_ALL) &&
			!(ntohs(flow->actions[i].arg.output.port) == OFPP_FLOOD)) {
			
			printk("---Port: %i---\n", ntohs(flow->actions[i].arg.output.port)); 
			printk("---Port is > 4 and not equal to ALL/FLOOD ---\n");
			return 0;		
		} 
	}
	return 1;
}

/*
 * Write all 0's out to an exact entry position
 */
void nf2_clear_of_exact(uint32_t pos) {
	nf2_of_entry_wrap entry;
	nf2_of_action_wrap action;
	struct net_device* dev = NULL;
		
	memset(&entry, 0, sizeof(nf2_of_entry_wrap));
	memset(&action, 0, sizeof(nf2_of_action_wrap));
	
	if ((dev = nf2_get_net_device())) {
		nf2_write_of_exact(dev, pos, &entry, &action);	
		nf2_free_net_device(dev);
	}			
}

/*
 * Write all 0's out to a wildcard entry position
 */
void nf2_clear_of_wildcard(uint32_t pos) {
	nf2_of_entry_wrap entry;
	nf2_of_mask_wrap mask;
	nf2_of_action_wrap action;
	struct net_device* dev = NULL;
	
	memset(&entry, 0, sizeof(nf2_of_entry_wrap));
	memset(&mask, 0, sizeof(nf2_of_mask_wrap));
	memset(&action, 0, sizeof(nf2_of_action_wrap));
	
	if ((dev = nf2_get_net_device())) {
		nf2_write_of_wildcard(dev, pos, &entry, &mask, &action);	
		nf2_free_net_device(dev);
	}			
}

int init_exact_free_list(void) {
	struct sw_flow_nf2* sfw = NULL;
	int i;
	for (i = 0; i < (OPENFLOW_NF2_EXACT_TABLE_SIZE); ++i) { 
	    sfw = kzalloc(sizeof(struct sw_flow_nf2), GFP_ATOMIC);
	    if (sfw == NULL) {
	    	return 1;
	    } 
		sfw->pos = i;
		sfw->type = NF2_TABLE_EXACT;
		add_free_exact(sfw);
		sfw = NULL;
	}
	
	return 0;	
}

int init_wildcard_free_list(void) {
	struct sw_flow_nf2* sfw = NULL;
	int i;
    INIT_LIST_HEAD(&wildcard_free_list);
	
	for (i = 0; i < (OPENFLOW_WILDCARD_TABLE_SIZE-8); ++i) { 
	    sfw = kzalloc(sizeof(struct sw_flow_nf2), GFP_ATOMIC);
	    if (sfw == NULL) {
	    	return 1;
	    } 
		sfw->pos = i;
		sfw->type = NF2_TABLE_WILDCARD;
		add_free_wildcard(sfw);
		sfw = NULL;
	}
	
	return 0;	
}

/*
 * Called when the table is being deleted
 */
void destroy_exact_free_list(void) {
	struct sw_flow_nf2* sfw = NULL;
	unsigned long int flags = 0;
	int i;
	
	for (i = 0; i < (OPENFLOW_NF2_EXACT_TABLE_SIZE); ++i) { 
	    sfw = exact_free_list[i];
	    if (sfw) {
	    	kfree(sfw);
	    }
	    sfw = NULL;
	}
}

/*
 * Called when the table is being deleted
 */
void destroy_wildcard_free_list(void) {
	struct sw_flow_nf2* sfw = NULL;
	struct list_head *next = NULL;
	
	unsigned long int flags = 0;
	int i;
	
	while(!list_empty(&wildcard_free_list)) {
		next = wildcard_free_list.next;
		sfw = list_entry(next, struct sw_flow_nf2, node);
		list_del(&sfw->node);
		kfree(sfw);		
	}
}

/* 
 * Setup the wildcard table by adding static flows that will handle 
 * misses by sending them up to the cpu ports, and handle packets coming 
 * back down from the cpu by sending them out the corresponding port.
 */
int nf2_write_static_wildcard(void) {
	nf2_of_entry_wrap entry;
	nf2_of_mask_wrap mask;
	nf2_of_action_wrap action;
	int i;
	struct net_device *dev;
	
	if ((dev = nf2_get_net_device())) {
		memset(&entry, 0x00, sizeof(nf2_of_entry_wrap));
		memset(&mask, 0xFF, sizeof(nf2_of_mask_wrap));
		// Only non-wildcard section is the source port
		mask.entry.src_port = 0;
		memset(&action, 0, sizeof(nf2_of_action_wrap));
		/*
		entry.entry.src_port = 0;
		entry.entry.ip_proto = 0x11;
		entry.entry.ip_src = 0x1;
		entry.entry.ip_dst = 0x2;
		entry.entry.eth_type = 0x800;
		entry.entry.eth_dst[5] = 0x2;
		entry.entry.eth_src[5] = 0x1;
		entry.entry.transp_src = 0x1;		
		entry.entry.vlan_id = 0xFFFF;
		
		action.action.forward_bitmask = 0x1 << (1*2);
		memset(&mask, 0xFF, sizeof(nf2_of_mask_wrap));

		nf2_write_of_wildcard(dev, 1, &entry, &mask, &action);		

		memset(&entry, 0x00, sizeof(nf2_of_entry_wrap));
		memset(&mask, 0xFF, sizeof(nf2_of_mask_wrap));
		// Only non-wildcard section is the source port
		mask.entry.src_port = 0;
		memset(&action, 0, sizeof(nf2_of_action_wrap));
		*/

		// write the catch all entries to send to the cpu
		for (i = 0; i < 4; ++i) {
			entry.entry.src_port = i*2;
			action.action.forward_bitmask = 0x1 << ((i*2)+1);
			nf2_write_of_wildcard(dev, 28+i, &entry, &mask, &action);		
		}
		
		// write the entries to send out packets coming from the cpu
		for (i = 0; i < 4; ++i) {
			entry.entry.src_port = (i*2)+1;
			action.action.forward_bitmask = 0x1 << (i*2);
			nf2_write_of_wildcard(dev, 24+i, &entry, &mask, &action);		
		}		
		
		nf2_free_net_device(dev);
		
		return 0;
	} else {
		return 1;
	}	
}

/*
 * Populate a nf2_of_entry_wrap with entries from a struct sw_flow
 */
void nf2_populate_of_entry(nf2_of_entry_wrap *key, struct sw_flow *flow) {
	int i;	
	key->entry.transp_dst = ntohs(flow->key.tp_dst);
	key->entry.transp_src = ntohs(flow->key.tp_src);
	key->entry.ip_proto = flow->key.nw_proto;
	key->entry.ip_dst = ntohl(flow->key.nw_dst);
	key->entry.ip_src = ntohl(flow->key.nw_src);
	key->entry.eth_type = ntohs(flow->key.dl_type);
	// Blame Jad for applying endian'ness to character arrays
	for (i=0; i<6; ++i) {
		key->entry.eth_dst[i] = flow->key.dl_dst[5-i];
	}
	for (i=0; i<6; ++i) {
		key->entry.eth_src[i] = flow->key.dl_src[5-i];
	}
	
	key->entry.src_port = ntohs(flow->key.in_port)*2;
	printk("Flow->in_port: %i, key->src_port: %i\n",ntohs(flow->key.in_port),
		key->entry.src_port);  
	key->entry.vlan_id = ntohs(flow->key.dl_vlan);
}

/*
 * Populate a nf2_of_mask_wrap with entries from a struct sw_flow's wildcards
 */
void nf2_populate_of_mask(nf2_of_mask_wrap *mask, struct sw_flow *flow) {
	int i;
		
	if (OFPFW_IN_PORT & flow->key.wildcards)
		mask->entry.src_port = 0xFF;
	if (OFPFW_DL_VLAN & flow->key.wildcards) {
		mask->entry.vlan_id = 0xFFFF;
	}
	if (OFPFW_DL_SRC & flow->key.wildcards) {
		for (i = 0; i < 6; ++i) {
			mask->entry.eth_src[i] = 0xFF;
		}
	}
	if (OFPFW_DL_DST & flow->key.wildcards) {
		for (i = 0; i < 6; ++i) {
			mask->entry.eth_dst[i] = 0xFF;
		}
	}
	if (OFPFW_DL_TYPE & flow->key.wildcards)
		mask->entry.eth_type = 0xFFFF;
	if (OFPFW_NW_SRC & flow->key.wildcards)
		mask->entry.ip_src = 0xFFFFFFFF;
	if (OFPFW_NW_DST & flow->key.wildcards)
		mask->entry.ip_dst = 0xFFFFFFFF;
	if (OFPFW_NW_PROTO & flow->key.wildcards)
		mask->entry.ip_proto = 0xFF;
	if (OFPFW_TP_SRC & flow->key.wildcards)
		mask->entry.transp_src = 0xFFFF;
	if (OFPFW_TP_DST & flow->key.wildcards)
		mask->entry.transp_dst = 0xFFFF;
	
	mask->entry.pad = 0x0000;
}

/*
 * Populate a nf2_of_mask_wrap with entries from a struct sw_flow's wildcards
 */
void nf2_populate_of_action(nf2_of_action_wrap *action, 
	nf2_of_entry_wrap *entry, nf2_of_mask_wrap *mask, struct sw_flow *flow) {
	unsigned short port = 0;
	int i, j;
	// zero it out for now	
	memset(action, 0, sizeof(nf2_of_action_wrap));
	printk("Number of actions: %i\n", flow->n_actions);	
	for (i=0; i < flow->n_actions; ++i) {
		if (flow->actions[i].type == OFPAT_OUTPUT) { 
			port = ntohs(flow->actions[i].arg.output.port);
			printk("Action Type: %i Output Port: %i\n",
				flow->actions[i].type, port);  
			
			if (port < 4)  {
				// bitmask for output port(s), evens are phys odds cpu
				action->action.forward_bitmask |= (1 << (port * 2));
				printk("Output Port: %i Forward Bitmask: %x\n",
					port, action->action.forward_bitmask);  
			} else if ((port == OFPP_ALL) || (port == OFPP_FLOOD)) {
				// Send out all ports except the source
				for (j = 0; j < 4; ++j) {
					if ((j*2) != entry->entry.src_port) {
						// bitmask for output port(s), evens are phys odds cpu
						action->action.forward_bitmask |= (1 << (j * 2));
					}
				}	
			} 
		}
	}
}

/*
 * Add a free hardware entry back to the exact pool
 */
void add_free_exact(struct sw_flow_nf2* sfw) {
	unsigned long int flags = 0;
	
	// clear the node entry
	INIT_LIST_HEAD(&sfw->node);
	
	// Critical section, adding to the actual list
	spin_lock_irqsave(&exact_free_lock, flags);
	exact_free_list[sfw->pos] = sfw;
	spin_unlock_irqrestore(&exact_free_lock, flags);
}

/*
 * Add a free hardware entry back to the wildcard pool
 */
void add_free_wildcard(struct sw_flow_nf2* sfw) {
	unsigned long int flags = 0;
	
	// Critical section, adding to the actual list
	spin_lock_irqsave(&wildcard_free_lock, flags);
	list_add_tail(&sfw->node, &wildcard_free_list);		
	spin_unlock_irqrestore(&wildcard_free_lock, flags);
}

/*
 * Hashes the entry to find where it should exist in the exact table
 * returns NULL on failure
 */
struct sw_flow_nf2* get_free_exact(nf2_of_entry_wrap *entry) {
	unsigned int poly1 = 0x04C11DB7;
	unsigned int poly2 = 0x1EDC6F41;
	struct sw_flow_nf2 *sfw = NULL;
	unsigned long int flags = 0;
	unsigned int hash = 0x0;
	unsigned int index = 0x0;
	
	struct crc32 crc;
	crc32_init(&crc, poly1);
	hash = crc32_calculate(&crc, entry, sizeof(nf2_of_entry_wrap));
	
	// the bottom 15 bits of hash == the index into the table
	index = 0x7FFF & hash;
	
	// if this index is free, grab it
	spin_lock_irqsave(&exact_free_lock, flags);
	sfw = exact_free_list[index];
	exact_free_list[index] = NULL;
	spin_unlock_irqrestore(&exact_free_lock, flags);
	
	if (sfw != NULL) {
		return sfw;
	}
	
	// try the second index
	crc32_init(&crc, poly2);
	hash = crc32_calculate(&crc, entry, sizeof(nf2_of_entry_wrap));
	// the bottom 15 bits of hash == the index into the table
	index = 0x7FFF & hash;
	
	// if this index is free, grab it
	spin_lock_irqsave(&exact_free_lock, flags);
	sfw = exact_free_list[index];
	exact_free_list[index] = NULL;
	spin_unlock_irqrestore(&exact_free_lock, flags);

	// return whether its good or not
	return sfw;
}

/*
 * Get the first free position in the wildcard hardware table
 * to write into 
 */
struct sw_flow_nf2* get_free_wildcard(void) {
	struct sw_flow_nf2 *sfw = NULL;
	struct list_head *next = NULL;
	unsigned long int flags = 0;
	
	// Critical section, pulling the first available from the list
	spin_lock_irqsave(&wildcard_free_lock, flags);
	if (list_empty(&wildcard_free_list)) {
		// empty :(
		sfw = NULL;
	} else {
		next = wildcard_free_list.next;
		sfw = list_entry(next, struct sw_flow_nf2, node);
		list_del_init(&sfw->node);		
	}
	spin_unlock_irqrestore(&wildcard_free_lock, flags);
	
	return sfw;
}

/*
 * Retrieves the type of table this flow should go into
 */
int nf2_get_table_type(struct sw_flow *flow) {
	if (flow->key.wildcards != 0) {
		printk("--- TABLE TYPE: WILDCARD ---\n");
		return NF2_TABLE_WILDCARD;
	} else {
		printk("--- TABLE TYPE: EXACT ---\n");
		return NF2_TABLE_EXACT;
	}
}

/*
 * Returns 1 if this flow contains an action outputting to all ports except 
 * input port, 0 otherwise. We support OFPP_ALL and OFPP_FLOOD actions, however
 * since we do not perform the spanning tree protocol (STP) then OFPP_FLOOD is 
 * equivalent to OFPP_ALL.
 */
int is_action_forward_all(struct sw_flow *flow) {
	int i;
	for (i=0; i < flow->n_actions; ++i) {
		// Currently only support the output port(s) action
		if ((flow->actions[i].type == OFPAT_OUTPUT) &&
			((flow->actions[i].arg.output.port == OFPP_ALL) || 
			 (flow->actions[i].arg.output.port == OFPP_FLOOD))) {
			
			return 1;		
		} 
	}
	
	return 0;
}

/*
 * Attempts to build and write the flow to hardware.
 * Returns 0 on success, 1 on failure.
 */
int nf2_build_and_write_flow(struct sw_flow *flow) {
    struct sw_flow_nf2 *sfw = NULL;
    struct sw_flow_nf2 *sfw_next = NULL;
    
    struct net_device *dev;
	int num_entries = 0;
	int i, table_type;
	nf2_of_entry_wrap key;
    nf2_of_mask_wrap mask;
    nf2_of_action_wrap action;
	
	memset(&key, 0, sizeof(nf2_of_entry_wrap));
	memset(&mask, 0, sizeof(nf2_of_mask_wrap));
	memset(&action, 0, sizeof(nf2_of_action_wrap));

	if (!(dev = nf2_get_net_device())) {
		// failure getting net device
		return 1;
	}

	table_type = nf2_get_table_type(flow);
	switch (table_type) {
		case NF2_TABLE_EXACT:
			printk("---Exact Entry---\n");
			nf2_populate_of_entry(&key, flow);
			nf2_populate_of_action(&action, &key, NULL, flow);
			sfw = get_free_exact(&key);
			if (sfw == NULL) {
				// collision
				return 1;
			}
			
			// set the active bit on this entry
			key.entry.pad = 0x8000;
			
			nf2_write_of_exact(dev, sfw->pos, &key, &action);

			flow->private = (void*)sfw;
			break;
		case NF2_TABLE_WILDCARD:
			printk("---Wildcard Entry---\n");
			// if action is all out and source port is wildcarded
			if ((is_action_forward_all(flow)) && 
				(flow->key.wildcards & OFPFW_IN_PORT)) {
				
				if (!(sfw = get_free_wildcard())) {
					// no free entries
					return 1;
				}
				// try to get 3 more positions
				for (i=0; i < 3; ++i) {
					if(!(sfw_next = get_free_wildcard())) {
						break;
					}				
					list_add_tail(&sfw_next->node, &sfw->node);
					++num_entries;	
				}
				
				if (num_entries < 3) {
					// failed to get enough entries, return them and exit
					nf2_delete_private((void*)sfw);
					return 1;
				}
				
				nf2_populate_of_entry(&key, flow);
				nf2_populate_of_mask(&mask, flow);
				
				// set first entry's src port to 0, remove wildcard mask on src
				key.entry.src_port = 0;
				mask.entry.src_port = 0;				
				nf2_populate_of_action(&action, &key, &mask, flow);
				nf2_write_of_wildcard(dev, sfw->pos, &key, &mask, &action);
				
				i = 1;
				sfw_next = list_entry(sfw->node.next, struct sw_flow_nf2, node);
				// walk through and write the remaining 3 entries
				while (sfw_next != sfw) {
					key.entry.src_port = i;
					nf2_populate_of_action(&action, &key, &mask, flow);
					nf2_write_of_wildcard(dev, sfw->pos, &key, &mask, &action);
					sfw_next = list_entry(sfw_next->node.next, 
						struct sw_flow_nf2, node);
					++i;
				}				
			} else {
				/* Get a free position here, and write to it */
				if ((sfw = get_free_wildcard())) {			
					nf2_populate_of_entry(&key, flow);
					nf2_populate_of_mask(&mask, flow);
					nf2_populate_of_action(&action, &key, &mask, flow);
					if (nf2_write_of_wildcard(dev, sfw->pos, &key, &mask, &action)) {
						// failure writing to hardware
						add_free_wildcard(sfw);
					    return 1;
					} else {
						// success writing to hardware, store the position
						flow->private = (void*)sfw;
					}			 
				} else {
					// hardware is full, return 0
					return 1;
				}
			}
			break;
	}

	nf2_free_net_device(dev);
	// success
	return 0;	
}

void nf2_delete_private(void* private) {
	struct sw_flow_nf2 *sfw = (struct sw_flow_nf2*)private;
	struct sw_flow_nf2 *sfw_next;
	struct list_head *next;
	
	switch (sfw->type) {
		case NF2_TABLE_EXACT:
			nf2_clear_of_exact(sfw->pos);
			add_free_exact(sfw);
			break;
		case NF2_TABLE_WILDCARD:
			while (!list_empty(&sfw->node)) {
				next = sfw->node.next;
				sfw_next = list_entry(next, struct sw_flow_nf2, node);
				list_del_init(&sfw_next->node);
				// Immediately zero out the entry in hardware
				nf2_clear_of_wildcard(sfw_next->pos);
				// add it back to the pool
				add_free_wildcard(sfw_next);
			}
		
			// zero the core entry
			nf2_clear_of_wildcard(sfw->pos);
			
			// add back the core entry
			add_free_wildcard(sfw);
		
			break;
	}
}

unsigned int nf2_get_packet_count(struct net_device *dev, struct sw_flow_nf2 *sfw) {
	unsigned int count = 0;
	switch (sfw->type) {
		case NF2_TABLE_EXACT:
			count = nf2_get_exact_packet_count(dev, sfw->pos);
			break;
		case NF2_TABLE_WILDCARD:
			count = nf2_get_wildcard_packet_count(dev, sfw->pos);
			break;	
	}	
	
	return count;
}

unsigned int nf2_get_byte_count(struct net_device *dev, struct sw_flow_nf2 *sfw) {
	unsigned int count = 0;
	switch (sfw->type) {
		case NF2_TABLE_EXACT:
			count = nf2_get_exact_byte_count(dev, sfw->pos);
			break;
		case NF2_TABLE_WILDCARD:
			count = nf2_get_wildcard_byte_count(dev, sfw->pos);
			break;	
	}	
	
	return count;
}

