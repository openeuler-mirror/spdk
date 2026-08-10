from .helpers import deprecated_alias
from getpass import getuser


def log_command_info(client, event):
    """log event info.
    Args:
        user_name: event user
        event: function id of PCI device
        src_addr: queue number of ssam
    """
    params = {
        'user_name': getuser(),
        'event': event,
        'src_addr': "localhost",
    }
    return client.call('log_command_info', params)


def log_info(func):
    def wrapper_log_info(arg, *args, **kw):
        log_command_info(arg.client, func.__name__)
        return func(arg, *args, **kw)
    return wrapper_log_info


def create_blk_controller(client, dev_name, index, readonly=None, serial=None):
    """Create ssam BLK controller.
    Args:
        dev_name: device name to add to controller
        index: function id or dbdf of PCI device
        queues: queue number of ssam
        readonly: set controller as read-only
        serial: set volume id
    """
    params = {
        'dev_name': dev_name,
        'index': index,
    }
    if readonly:
        params['readonly'] = readonly
    if serial:
        params['serial'] = serial
    return client.call('create_blk_controller', params)


def get_controllers(client, function_id=None, dbdf=None):
    """Get information about configured ssam controllers.

    Args:
        function_id: function id of PCI device
        dbdf: dbdf of PCI device

    Returns:
        List of ssam controllers.
    """
    params = {}
    if function_id is not None:
        params['function_id'] = function_id
    if dbdf is not None:
        params['dbdf'] = dbdf
    return client.call('get_controllers', params)


def get_scsi_controllers(client, name=None):
    """Get information about configured ssam controllers.

    Args:
        name: name of scsi controller

    Returns:
        List of ssam scsi controllers.
    """
    params = {}
    if name is not None:
        params['name'] = name
    return client.call('get_scsi_controllers', params)


def delete_controller(client, index):
    """Delete ssam controller from configuration.
    Args:
        index: function id or dbdf of PCI device
    """
    params = {'index': index}
    return client.call('delete_controller', params)


def delete_scsi_controller(client, name):
    """Delete ssam controller from configuration.
    Args:
        name: scsi controller name to be delete
    """
    params = {'name': name}
    return client.call('delete_scsi_controller', params)


def controller_get_iostat(client, function_id=None, dbdf=None):
    """Get iostat about configured ssam controllers.

    Args:
        function_id: function id of PCI device
        dbdf: dbdf of PCI device

    Returns:
        List of iostat of ssam controllers.
    """
    params = {}
    if function_id is not None:
        params['function_id'] = function_id
    if dbdf is not None:
        params['dbdf'] = dbdf
    return client.call('controller_get_iostat', params)


def controller_clear_iostat(client, type=None):
    """Clear iostat about configured ssam controllers.

    Args:
        type: blk,scsi,fs

    """
    params = {}
    if type is not None:
        params['type'] = type
    return client.call('controller_clear_iostat', params)


def bdev_resize(client, function_id, new_size_in_mb):
    """Resize bdev in the system.
    Args:
        function_id: function id of PCI device
        new_size_in_mb: new bdev size for resize operation. The unit is MiB
    """
    params = {
        'function_id': function_id,
        'new_size_in_mb': new_size_in_mb,
    }
    return client.call('bdev_resize', params)


def scsi_bdev_resize(client, name, tgt_id, new_size_in_mb):
    """Resize scsi bdev in the system.
    Args:
        name: controller name of PCI device
        tgt_id: tgt id of bdev
        new_size_in_mb: new bdev size for resize operation. The unit is MiB
    """
    params = {
        'name': name,
        'tgt_id': tgt_id,
        'new_size_in_mb': new_size_in_mb,
    }
    return client.call('scsi_bdev_resize', params)


def bdev_aio_resize(client, name, new_size_in_mb):
    """Resize aio bdev in the system.
    Args:
        name: aio bdev name
        new_size_in_mb: new bdev size for resize operation. The unit is MiB
    """
    params = {
        'name': name,
        'new_size_in_mb': new_size_in_mb,
    }
    return client.call('bdev_aio_resize', params)


def os_ready(client):
    """Write ready flag for booting OS.

    """
    return client.call('os_ready')


def create_scsi_controller(client, dbdf, name):
    """Create ssam scsi controller.
    Args:
        dbdf: the pci dbdf of virtio scsi controller
        name: controller name to be create
    """
    params = {
        'dbdf': dbdf,
        'name': name,
    }

    return client.call('create_scsi_controller', params)


def scsi_controller_add_target(client, name, scsi_tgt_num, bdev_name):
    """Add LUN to ssam scsi controller target.
    Args:
        name: controller name where add lun
        scsi_tgt_num: target number to use
        bdev_name: name of bdev to add to target
    """
    params = {
        'name': name,
        'scsi_tgt_num': scsi_tgt_num,
        'bdev_name': bdev_name,
    }
    return client.call('scsi_controller_add_target', params)


def scsi_controller_remove_target(client, name, scsi_tgt_num):
    """Remove LUN from ssam scsi controller target.
    Args:
        name: controller name to remove lun
        scsi_tgt_num: target number to use
    """
    params = {
        'name': name,
        'scsi_tgt_num': scsi_tgt_num,
    }
    return client.call('scsi_controller_remove_target', params)


def scsi_device_iostat(client, name, scsi_tgt_num):
    """Get iostat about scsi device.

    Args:
        name: controller name
        scsi_tgt_num: target number

    Returns:
        List of iostat of ssam controllers.
    """
    params = {
        'name': name,
        'scsi_tgt_num': scsi_tgt_num,
    }
    return client.call('scsi_device_iostat', params)


def device_pcie_list(client):
    """Show storage device pcie list.

    Returns:
        List of storage device pcie.
    """

    return client.call('device_pcie_list')

def get_ssam_info(client):
    """Get information.

    Returns:
        List of IO statistics information
    """
    return client.call('get_ssam_info')

def set_crc_checklog(client, toggle):
    """Set io crc check enable or disable.
    Args:
        toggle: enable or disable
    """
    params = {'toggle': toggle}
    return client.call('set_crc_checklog', params)