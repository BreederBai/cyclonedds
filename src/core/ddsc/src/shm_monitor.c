/*
 * Copyright(c) 2021 Apex.AI Inc. All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */


#include "shm__monitor.h"

#include "dds__types.h"
#include "dds__entity.h"
#include "dds__reader.h"

#include "dds/ddsi/q_entity.h"
#include "dds/ddsi/ddsi_entity_index.h"
#include "dds/ddsi/q_xmsg.h"
#include "dds/ddsi/ddsi_tkmap.h"
#include "dds/ddsi/ddsi_rhc.h"

#if defined (__cplusplus)
extern "C" {
#endif

static void shm_wakeup_trigger_callback(iox_user_trigger_t trigger);
static void shm_subscriber_callback(iox_sub_t subscriber);

void shm_monitor_init(shm_monitor_t* monitor) {
    ddsrt_mutex_init(&monitor->m_lock);

    monitor->m_listener = iox_listener_init(&monitor->m_listener_storage);
    monitor->m_wakeup_trigger = iox_user_trigger_init(&monitor->m_wakeup_trigger_storage.storage);
    monitor->m_wakeup_trigger_storage.monitor = monitor;
    iox_listener_attach_user_trigger_event(monitor->m_listener, monitor->m_wakeup_trigger, shm_wakeup_trigger_callback);

    monitor->m_state = SHM_MONITOR_RUNNING;
}

void shm_monitor_destroy(shm_monitor_t* monitor) {
    //note: we must ensure no readers are actively using the monitor anymore,    
    shm_monitor_wake_and_disable(monitor);
    while(monitor->m_state == SHM_MONITOR_RUNNING); //spin until the callback thread is no longer running

    //ICEORYX_TODO: is it ok to deinit while readers are still attached?
    //              they should be automatically detached
    iox_listener_deinit(monitor->m_listener);
    ddsrt_mutex_destroy(&monitor->m_lock);
}

dds_return_t shm_monitor_wake_and_invoke(shm_monitor_t* monitor, void (*function) (void*), void* arg) {
    iox_user_trigger_storage_extension_t* storage = (iox_user_trigger_storage_extension_t*) monitor->m_wakeup_trigger;
    storage->call = function;
    storage->arg = arg;
    iox_user_trigger_trigger(monitor->m_wakeup_trigger);
    return DDS_RETCODE_OK;
}

dds_return_t shm_monitor_wake_and_disable(shm_monitor_t* monitor) {
    monitor->m_state = SHM_MONITOR_NOT_RUNNING;
    iox_user_trigger_trigger(monitor->m_wakeup_trigger);
    return DDS_RETCODE_OK;
}

dds_return_t shm_monitor_wake_and_enable(shm_monitor_t* monitor) {
    monitor->m_state = SHM_MONITOR_RUNNING;
    iox_user_trigger_trigger(monitor->m_wakeup_trigger);
    return DDS_RETCODE_OK;
}

dds_return_t shm_monitor_attach_reader(shm_monitor_t* monitor, struct dds_reader* reader) {

    if(iox_listener_attach_subscriber_event(monitor->m_listener, reader->m_iox_sub, SubscriberEvent_HAS_DATA, shm_subscriber_callback) != ListenerResult_SUCCESS) {
        DDS_CLOG(DDS_LC_SHM, &reader->m_rd->e.gv->logconfig, "error attaching reader\n");    
        return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    ++monitor->m_number_of_attached_readers;

    return DDS_RETCODE_OK;
}

dds_return_t shm_monitor_detach_reader(shm_monitor_t* monitor, struct dds_reader* reader) {
    iox_listener_detach_subscriber_event(monitor->m_listener, reader->m_iox_sub, SubscriberEvent_HAS_DATA); 
    --monitor->m_number_of_attached_readers;
    return DDS_RETCODE_OK;
}

static void receive_data_wakeup_handler(struct dds_reader* rd)
{
  void* chunk = NULL;
  thread_state_awake(lookup_thread_state(), rd->m_rd->e.gv);

  while (true)
  {
    //ICEORYX_TODO mutex could be more fine grained, one per subscriber
    shm_mutex_lock();
    enum iox_ChunkReceiveResult take_result = iox_sub_take_chunk(rd->m_iox_sub, (const void** const)&chunk);
    shm_mutex_unlock();
    if (ChunkReceiveResult_SUCCESS != take_result)
      break;

    iceoryx_header_t* ice_hdr = (iceoryx_header_t*)chunk;
    // Get proxy writer
    struct proxy_writer* pwr = entidx_lookup_proxy_writer_guid(rd->m_rd->e.gv->entity_index, &ice_hdr->guid);
    if (pwr == NULL)
    {
      // We should ignore chunk which does not match the pwr in receiver side.
      // For example, intra-process has local pwr and does not need to use iceoryx, so we can ignore it.
      DDS_CLOG(DDS_LC_SHM, &rd->m_rd->e.gv->logconfig, "pwr is NULL and we'll ignore.\n");
      continue;
    }

    // Create struct ddsi_serdata
    struct ddsi_serdata* d = ddsi_serdata_from_iox(rd->m_topic->m_stype, ice_hdr->data_kind, &rd->m_iox_sub, chunk);
    //keyhash needs to be set here
    d->timestamp.v = ice_hdr->tstamp;

    // Get struct ddsi_tkmap_instance
    struct ddsi_tkmap_instance* tk;
    if ((tk = ddsi_tkmap_lookup_instance_ref(rd->m_rd->e.gv->m_tkmap, d)) == NULL)
    {
      DDS_CLOG(DDS_LC_SHM, &rd->m_rd->e.gv->logconfig, "ddsi_tkmap_lookup_instance_ref failed.\n");
      goto release;
    }

    // Generate writer_info
    struct ddsi_writer_info wrinfo;
    ddsi_make_writer_info(&wrinfo, &pwr->e, pwr->c.xqos, d->statusinfo);

    (void)ddsi_rhc_store(rd->m_rd->rhc, &wrinfo, d, tk);

release:
    if (tk)
      ddsi_tkmap_instance_unref(rd->m_rd->e.gv->m_tkmap, tk);
    if (d)
      ddsi_serdata_unref(d);
  }
  thread_state_asleep(lookup_thread_state());
}

static void shm_wakeup_trigger_callback(iox_user_trigger_t trigger) {    
    // we know it is actually in extended storage since we created it like this
    iox_user_trigger_storage_extension_t* storage = (iox_user_trigger_storage_extension_t*) trigger;
    if(storage->monitor->m_state == SHM_MONITOR_RUNNING && storage->call) {
        storage->call(storage->arg);
    }
}

static void shm_subscriber_callback(iox_sub_t subscriber) {
    // we know it is actually in extended storage since we created it like this
    iox_sub_storage_extension_t *storage = (iox_sub_storage_extension_t*) subscriber; 
    if(storage->monitor->m_state == SHM_MONITOR_RUNNING) {
        receive_data_wakeup_handler(storage->parent_reader);
    }
}

#if defined (__cplusplus)
}
#endif
