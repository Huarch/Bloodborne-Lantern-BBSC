"""Add the server-status schema requested by the installed Asian Bloodborne executable.

The unmodified client reported selector=4, language=12 and rejected the stock
EU-only selector=2 status document. This extends the server response; it does
not bypass the client's parser or modify game code.
"""
import base64
import copy
import re
import xml.etree.ElementTree as ET

import bbsc_gameserver as server

original_build = server.build_ss_info


def adapt_status(encoded):
    root = ET.fromstring('<root>' + base64.b64decode(encoded).decode('utf-8') + '</root>')
    # Add language 12 only where the stock server supplies localized fields.
    for element in list(root.iter()):
        localized = [child for child in element if re.fullmatch(r'lang\d+', child.tag)]
        if localized and element.find('lang12') is None:
            fallback = element.find('lang3')
            if fallback is None:
                fallback = localized[0]
            extra = copy.deepcopy(fallback)
            extra.tag = 'lang12'
            element.append(extra)
    # Top-level numeric suffixes select the status schema, not the game's FPS.
    for element in list(root):
        if element.tag.endswith('2'):
            target = element.tag[:-1] + '4'
            if root.find(target) is None:
                extra = copy.deepcopy(element)
                extra.tag = target
                root.append(extra)
    text = '\n'.join(ET.tostring(child, encoding='unicode', short_empty_elements=False) for child in root)
    assert root.find('gameurl4') is not None
    assert root.find('msg4/lang12') is not None
    assert root.find('ReloadServerStatusInfoInterval4') is not None
    return base64.b64encode(text.encode('utf-8'))


def build_ss_info(local=False, quiet=False, bbhost=False):
    return adapt_status(original_build(local=local, quiet=quiet, bbhost=bbhost))


server.build_ss_info = build_ss_info
if __name__ == '__main__':
    print('Lantern compatibility: retaining schema 2 and adding schema 4 / language 12.', flush=True)
    server.main()
