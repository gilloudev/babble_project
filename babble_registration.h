#ifndef __BABBLE_REGISTRATION_H__
#define __BABBLE_REGISTRATION_H__

#include "babble_types.h"

/* array storing a pointer to each register client */
extern client_data_t *registration_table[MAX_CLIENT];

/* number of registered clients */
extern int nb_registered_clients;

/* initialize the table*/
void registration_init(void);

/* search for client corresponding to key */
client_data_t* registration_lookup(unsigned long key);

/* insert client */
int registration_insert(client_data_t* cl);

/* remove client from the registration table */
client_data_t* registration_remove(unsigned long key);


#endif
