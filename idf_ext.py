"""Reject direct SDK device writes before its automatic serial-port selection."""
BLOCKED = frozenset(('flash', 'app-flash', 'bootloader-flash', 'partition-table-flash',
                     'encrypted-flash', 'encrypted-app-flash', 'erase-flash', 'erase_flash', 'erase-otadata'))
MESSAGE = ('Direct SDK flash/erase is disabled for this project. Use tools/satori_dev.py usb-upgrade '
           'after layout checking; factory devices require explicit tools/migrate_layout.py. '
           'Builds never migrate devices. Manual external esptool is not intercepted.')

def guard(tasks):
    if any(task.name in BLOCKED for task in tasks):
        raise RuntimeError(MESSAGE)

def check_actions(ctx, args, tasks):
    from idf_py_actions.errors import FatalError
    try: guard(tasks)
    except RuntimeError: raise FatalError(MESSAGE) from None

def action_extensions(base_actions, project_path):
    return {'global_action_callbacks': [check_actions]}
