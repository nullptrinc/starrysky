#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""List the shapes of each Inkscape layer or labelled group in an SVG sheet.

Prints "<label> <shape id>" per drawn shape, where nested labels are joined
with '/', e.g. "percents_white/4 path30". Starboard renders icons from such
a sheet with NanoSVG, which knows shape ids but not Inkscape labels.

    tools/svg-layers.py data/icons/battery.svg > data/icons/battery.map
"""
import sys
import xml.etree.ElementTree as ET

SVG = '{http://www.w3.org/2000/svg}'
LABEL = '{http://www.inkscape.org/namespaces/inkscape}label'
SHAPES = {f'{SVG}{t}' for t in ('path', 'rect', 'circle', 'ellipse', 'polygon', 'polyline', 'line')}


def walk(element, labels, out):
    if element.tag == f'{SVG}defs':
        return
    label = element.get(LABEL)
    if element.tag == f'{SVG}g':
        # Inkscape layers name the sheet, not the icon part; skip them.
        is_layer = element.get('{http://www.inkscape.org/namespaces/inkscape}groupmode') == 'layer'
        inner = labels if is_layer or not label else labels + [label]
        for child in element:
            walk(child, inner, out)
    elif element.tag == f'{SVG}svg':
        for child in element:
            walk(child, labels, out)
    elif element.tag in SHAPES and element.get('id'):
        name = '/'.join(labels + [label]) if labels and label and labels[-1].startswith('percents') \
            else '/'.join(labels) if labels else label
        if name:
            out.append((name, element.get('id')))


def main():
    out = []
    walk(ET.parse(sys.argv[1]).getroot(), [], out)
    for name, shape in out:
        print(name, shape)


if __name__ == '__main__':
    main()
