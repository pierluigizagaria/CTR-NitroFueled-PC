# SPDX-License-Identifier: GPL-2.0-or-later
"""Use the player's verified shader cache; no original game shaders are bundled."""
import hashlib
import os
from pathlib import Path
import uuid


def update_shaders(cache, backup, enabled, factor, signatures, centered_shader,
                   known_ultrawide_shader):
    cache, backup = Path(cache), Path(backup)
    operations = []
    for shader, signature in signatures.items():
        name = f'0x00000000{shader}_0.spv'
        path, saved = cache / name, backup / name
        if not enabled and not saved.exists():
            # Do not change a cache this integration has never modified.
            continue
        if not path.is_file():
            if enabled:
                raise RuntimeError('UltraWide needs a warm HUD shader cache; launch in 16:9 first.')
            continue
        current = path.read_bytes()
        original = saved.read_bytes() if saved.is_file() else current
        if hashlib.sha256(original).hexdigest() != signature:
            raise RuntimeError(f'UltraWide: original HUD shader {shader} is not recognized. '
                               'Restore an unmodified local shader cache before enabling UltraWide; '
                               'no shader files were changed.')
        if not known_ultrawide_shader(current, original):
            raise RuntimeError(f'UltraWide: another modification uses HUD shader {shader}; '
                               'no shader files were changed.')
        desired = centered_shader(original, factor) if enabled else original
        operations.append((path, saved, original, current, desired))

    # Validate the complete set before backing up or modifying either shader.
    for path, saved, original, current, desired in operations:
        if not saved.exists():
            backup.mkdir(parents=True, exist_ok=True)
            with saved.open('xb') as stream:
                stream.write(original)
        if current != desired:
            temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
            try:
                temporary.write_bytes(desired)
                os.replace(temporary, path)
            finally:
                temporary.unlink(missing_ok=True)
