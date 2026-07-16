#include "dmx/include/service.h"

#include <string.h>

#include "dmx/include/driver.h"

void dmx_rx_snapshot_request_locked(dmx_driver_t *driver,
                                    size_t snapshot_size) {
  if (driver == NULL) {
    return;
  }

  driver->rx_snapshot.request_active = snapshot_size > 0;
  driver->rx_snapshot.pending = false;
  driver->rx_snapshot.requested_size = snapshot_size;
  driver->rx_snapshot.copied_size = 0;

  // A packet completed before registration has no same-transaction retained
  // prefix. Do not reconstruct it from the asynchronously reusable live data.
  if (driver->dmx.progress == DMX_PROGRESS_COMPLETE) {
    driver->dmx.progress = DMX_PROGRESS_STALE;
  }
}

bool DMX_ISR_ATTR dmx_rx_snapshot_capture_locked(dmx_driver_t *driver,
                                                 int packet_size,
                                                 dmx_err_t err) {
  if (driver == NULL || !driver->rx_snapshot.request_active ||
      driver->rx_snapshot.pending) {
    return false;
  }

  const size_t normalized_size = packet_size > 0 ? (size_t)packet_size : 0;
  size_t copied_size = driver->rx_snapshot.requested_size;
  if (copied_size > normalized_size) {
    copied_size = normalized_size;
  }
  if (copied_size > DMX_PACKET_SIZE_MAX) {
    copied_size = DMX_PACKET_SIZE_MAX;
  }

  // Use a simple bounded loop instead of calling memcpy() from the ISR path.
  for (size_t i = 0; i < copied_size; ++i) {
    driver->rx_snapshot.data[i] = driver->dmx.data[i];
  }

  driver->rx_snapshot.copied_size = copied_size;
  driver->rx_snapshot.packet.err = err;
  driver->rx_snapshot.packet.sc =
      normalized_size > 0 ? driver->rx_snapshot.data[0] : -1;
  driver->rx_snapshot.packet.size = normalized_size;
  driver->rx_snapshot.packet.is_rdm =
      dmx_start_code_is_rdm(driver->rx_snapshot.packet.sc);
  driver->rx_snapshot.pending = true;
  return true;
}

#ifdef PIO_UNIT_TESTING
size_t dmx_test_rx_snapshot_retention(
    const uint8_t *first_packet, size_t first_packet_size,
    dmx_err_t first_packet_err, const uint8_t *second_packet,
    size_t second_packet_size, void *destination, size_t snapshot_size,
    dmx_packet_t *packet) {
  if (first_packet == NULL || second_packet == NULL || destination == NULL ||
      packet == NULL || first_packet_size > DMX_PACKET_SIZE_MAX ||
      second_packet_size > DMX_PACKET_SIZE_MAX || snapshot_size == 0 ||
      snapshot_size > DMX_PACKET_SIZE_MAX) {
    return 0;
  }

  dmx_driver_t driver = {0};

  // A completion that predates registration must be discarded instead of
  // being reconstructed from the live buffer.
  memcpy(driver.dmx.data, second_packet, second_packet_size);
  driver.dmx.head = (int)second_packet_size;
  driver.dmx.progress = DMX_PROGRESS_COMPLETE;
  dmx_rx_snapshot_request_locked(&driver, snapshot_size);
  if (driver.dmx.progress != DMX_PROGRESS_STALE ||
      driver.rx_snapshot.pending) {
    return 0;
  }

  memcpy(driver.dmx.data, first_packet, first_packet_size);
  if (!dmx_rx_snapshot_capture_locked(&driver, (int)first_packet_size,
                                      first_packet_err)) {
    return 0;
  }

  // Reuse the live buffer exactly as the next UART frame would. The pending
  // first snapshot must prevent this second completion from replacing it.
  memcpy(driver.dmx.data, second_packet, second_packet_size);
  if (dmx_rx_snapshot_capture_locked(&driver, (int)second_packet_size,
                                     DMX_OK)) {
    return 0;
  }

  memset(destination, 0, snapshot_size);
  memcpy(destination, driver.rx_snapshot.data,
         driver.rx_snapshot.copied_size);
  *packet = driver.rx_snapshot.packet;
  return driver.rx_snapshot.copied_size;
}
#endif

dmx_device_t *dmx_device_get(dmx_port_t dmx_num, dmx_device_num_t device_num) {
  assert(dmx_num < DMX_NUM_MAX);
  assert(device_num < RDM_SUB_DEVICE_MAX);
  assert(dmx_driver_is_installed(dmx_num));

  dmx_device_t *device = &dmx_driver[dmx_num]->device.root;
  while (device->num != device_num) {
    device = device->next;
    if (device == NULL) {
      return NULL;  // Sub-device does not exist
    }
  }

  return device;
}

bool dmx_parameter_add(dmx_port_t dmx_num, dmx_device_num_t device_num,
                       rdm_pid_t pid, int type, void *data, size_t size) {
  assert(dmx_num < DMX_NUM_MAX);
  assert(device_num < RDM_SUB_DEVICE_MAX);
  assert(pid > 0);
  assert(dmx_driver_is_installed(dmx_num));

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Find the sub-device
  dmx_device_t *device = dmx_device_get(dmx_num, device_num);
  if (device == NULL) {
    return false;  // Device does not exist
  }

  // Iterate through parameters until an empty parameter is found
  const uint32_t parameter_count =
      device_num == RDM_SUB_DEVICE_ROOT
          ? driver->device.parameter_count.root
          : driver->device.parameter_count.sub_devices;
  for (int i = 0; i < parameter_count; ++i) {
    if (device->parameters[i].pid == pid) {
      return true;  // Parameter already exists
    } else if (device->parameters[i].pid == 0) {
      // Initialize parameter memory
      switch (type) {
        case DMX_PARAMETER_TYPE_DYNAMIC:
        case DMX_PARAMETER_TYPE_NON_VOLATILE:
          device->parameters[i].data = malloc(size);
          if (device->parameters[i].data == NULL) {
            DMX_ERR("parameter malloc error");
            return false;
          }
          if (data == NULL) {
            memset(device->parameters[i].data, 0, size);
          } else {
            memcpy(device->parameters[i].data, data, size);
          }
          break;
        case DMX_PARAMETER_TYPE_STATIC:
          device->parameters[i].data = data;
          break;
        case DMX_PARAMETER_TYPE_NULL:
          device->parameters[i].data = NULL;
          break;
        default:
          return false;
      }

      device->parameters[i].pid = pid;
      device->parameters[i].size = size;
      device->parameters[i].type = type;
      device->parameters[i].definition = NULL;
      device->parameters[i].callback = NULL;
      return true;
    }
  }

  return false;  // No more parameters available on this sub-device
}

dmx_parameter_t *dmx_parameter_get_entry(dmx_port_t dmx_num,
                                         dmx_device_num_t device_num,
                                         rdm_pid_t pid) {
  assert(dmx_num < DMX_NUM_MAX);
  assert(device_num < RDM_SUB_DEVICE_MAX);
  assert(pid > 0);
  assert(dmx_driver_is_installed(dmx_num));

  dmx_device_t *const device = dmx_device_get(dmx_num, device_num);
  if (device == NULL) {
    return NULL;  // Sub-device does not exist
  }

  // Get the parameter count for the requested device
  const int param_count =
      (device_num == RDM_SUB_DEVICE_ROOT)
          ? dmx_driver[dmx_num]->device.parameter_count.root
          : dmx_driver[dmx_num]->device.parameter_count.sub_devices;

  // Iterate the device's parameters
  for (int i = 0; i < param_count; ++i) {
    if (device->parameters[i].pid == pid) {
      return &device->parameters[i];
    }
  }

  return NULL;  // Parameter does not exist
}
