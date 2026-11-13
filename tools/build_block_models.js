// Resolve Java models on the build machine; PSP loads only bounded binary tables.
const fs = require('fs');
const path = require('path');
const root = path.resolve(__dirname, '..');
const resources = path.resolve(process.argv[2] || path.join(root, 'assets/imported'));
const registryPath = path.resolve(process.argv[3] || path.join(root, '.deps/mc26-data/blocks.json'));
const base = path.join(resources, 'assets/minecraft');
const out = path.join(resources, 'compiled');
const opacityPath = path.join(out, 'texture-opacity.json');
const opacity = fs.existsSync(opacityPath) ? JSON.parse(fs.readFileSync(opacityPath, 'utf8')) : {};
const registry = JSON.parse(fs.readFileSync(registryPath, 'utf8'));
const behaviour = JSON.parse(fs.readFileSync(path.join(path.dirname(registryPath), 'block_behaviour.json'), 'utf8'));
const directions = ['north', 'south', 'west', 'east', 'up', 'down'];
const normals = [[0,0,-1], [0,0,1], [-1,0,0], [1,0,0], [0,1,0], [0,-1,0]];
const modelCache = new Map();
const compiledCache = new Map();
const textureNames = [];
const textureIds = new Map();
const faceIds = new Map();
const faces = [];
const references = [];
const modelRanges = new Map();
const boxes = [];
const shapes = [];
fs.mkdirSync(out, { recursive: true });

function readJson(file)
{
    return fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, 'utf8')) : {};
}

function model(name, depth = 0)
{
    name = name.replace(/^minecraft:/, '');
    if (modelCache.has(name))
    {
        return modelCache.get(name);
    }
    if (depth > 20)
    {
        throw new Error('Cyclic model: ' + name);
    }
    const own = readJson(path.join(base, 'models', name + '.json'));
    const parent = own.parent ? model(own.parent, depth + 1) : {};
    const result = { ...parent, ...own, textures: { ...parent.textures, ...own.textures } };
    modelCache.set(name, result);
    return result;
}

function texture(name, textures)
{
    for (let depth = 0; name && name[0] === '#' && depth < 20; ++depth)
    {
        name = textures[name.slice(1)];
    }
    if (name && typeof name === 'object')
    {
        name = name.sprite;
    }
    name = (name || 'block/stone').replace(/^minecraft:/, '');
    if (!fs.existsSync(path.join(base, 'textures', name + '.png')))
    {
        name = 'block/stone';
    }
    if (!textureIds.has(name))
    {
        textureIds.set(name, textureNames.length);
        textureNames.push(name);
    }
    return textureIds.get(name);
}

function rotate(p, axis, degrees, origin = [8,8,8], rescale = false)
{
    const a = degrees * Math.PI / 180;
    const c = Math.cos(a);
    const s = Math.sin(a);
    const q = p.map((v, i) => v - origin[i]);
    const [u, v] = axis === 'x' ? [1,2] : axis === 'y' ? [2,0] : [0,1];
    const first = q[u] * c - q[v] * s;
    const second = q[u] * s + q[v] * c;
    const scale = rescale ? 1 / Math.abs(c) : 1;
    q[u] = first * scale;
    q[v] = second * scale;
    return q.map((value, i) => value + origin[i]);
}

function stateRotate(p, entry, origin = [8,8,8])
{
    p = rotate(p, 'x', -(entry.x || 0), origin);
    return rotate(p, 'y', -(entry.y || 0), origin);
}

function sideOf(n)
{
    return normals.findIndex(v => v.every((c, i) => Math.abs(c - n[i]) < 0.001));
}

function vertices(side, a, b)
{
    const [x,y,z] = a;
    const [X,Y,Z] = b;
    return [
        [[X,y,z],[X,Y,z],[x,Y,z],[x,y,z]],
        [[x,y,Z],[x,Y,Z],[X,Y,Z],[X,y,Z]],
        [[x,y,z],[x,Y,z],[x,Y,Z],[x,y,Z]],
        [[X,y,Z],[X,Y,Z],[X,Y,z],[X,y,z]],
        [[x,Y,Z],[x,Y,z],[X,Y,z],[X,Y,Z]],
        [[x,y,z],[x,y,Z],[X,y,Z],[X,y,z]]
    ][side].reverse();
}

function defaultUv(side, a, b)
{
    const [x,y,z] = a;
    const [X,Y,Z] = b;
    return [
        [16-X,16-Y,16-x,16-y], [x,16-Y,X,16-y],
        [z,16-Y,Z,16-y], [16-Z,16-Y,16-z,16-y],
        [x,z,X,Z], [x,16-Z,X,16-z]
    ][side];
}

function matches(condition, properties)
{
    if (!condition)
    {
        return true;
    }
    return Object.entries(condition).every(([key, value]) =>
    {
        if (key === 'OR')
        {
            return value.some(c => matches(c, properties));
        }
        if (key === 'AND')
        {
            return value.every(c => matches(c, properties));
        }
        return String(value).split('|').includes(properties[key]);
    });
}

function select(entry)
{
    return Array.isArray(entry) ? entry[0] : entry;
}

function compile(entry, tint)
{
    const key = JSON.stringify(entry) + ':' + tint;
    if (compiledCache.has(key))
    {
        return compiledCache.get(key);
    }
    const resolved = model(entry.model);
    const result = [];
    for (const element of resolved.elements || [])
    {
        for (const [direction, face] of Object.entries(element.faces || {}))
        {
            const side = directions.indexOf(direction);
            if (side < 0)
            {
                continue;
            }
            let points = vertices(side, element.from, element.to);
            if (element.rotation)
            {
                const r = element.rotation;
                points = points.map(p => rotate(p, r.axis, r.angle, r.origin, r.rescale));
            }
            points = points.map(p => stateRotate(p, entry));
            const edge = points[1].map((v, i) => v - points[0][i]);
            const other = points[2].map((v, i) => v - points[0][i]);
            let normal = [edge[1]*other[2]-edge[2]*other[1], edge[2]*other[0]-edge[0]*other[2], edge[0]*other[1]-edge[1]*other[0]];
            const length = Math.hypot(...normal);
            if (length < 0.00001)
            {
                continue;
            }
            normal = normal.map(v => v / length);
            const uv = face.uv || defaultUv(side, element.from, element.to);
            let coords = [[uv[2],uv[3]],[uv[2],uv[1]],[uv[0],uv[1]],[uv[0],uv[3]]];
            const turns = (face.rotation || 0) / 90;
            coords = coords.map((_, i) => coords[(i + turns) % 4]);
            if (entry.uvlock && !element.rotation)
            {
                const destSide = sideOf(normal);
                if (destSide >= 0)
                {
                    const standard = vertices(destSide, [0,0,0], [16,16,16]);
                    const source = vertices(side, [0,0,0], [16,16,16]).map(p => stateRotate(p, entry));
                    // Transform coordinates around the full texture, not around the cropped
                    // UV rectangle: permuting a half-height rectangle stretches stair sides.
                    const fullUv = [[16,16],[16,0],[0,0],[0,16]];
                    const target = source.map(p => fullUv[standard.findIndex(q => q.every((v, i) => Math.abs(v-p[i]) < 0.001))]);
                    coords = coords.map(([u,v]) => [0,1].map(axis => target[2][axis]
                        + (target[1][axis] - target[2][axis]) * u / 16
                        + (target[3][axis] - target[2][axis]) * v / 16));
                }
            }
            const tile = texture(face.texture, resolved.textures || {});
            const cull = face.cullface ? sideOf(stateRotate(normals[directions.indexOf(face.cullface)], entry, [0,0,0])) : -1;
            result.push({ points, coords, normal, cull, tile, tint: face.tintindex !== undefined ? tint : 0, shade: element.shade === false || element.shade_direction_override === 'up' ? 255 : 0 });
        }
    }
    compiledCache.set(key, result);
    return result;
}

function packedFace(face)
{
    const data = Buffer.alloc(40);
    face.points.flat().forEach((value, i) => data.writeInt16LE(Math.round(value * 16), i * 2));
    face.coords.flat().forEach((value, i) => data.writeUInt8(Math.max(0, Math.min(255, Math.round(value * 255 / 16))), 24 + i));
    data.writeUInt16LE(face.tile, 32);
    face.normal.forEach((v, i) => data.writeInt8(Math.round(v * 127), 34 + i));
    data.writeInt8(face.cull, 37);
    data[38] = face.tint;
    data[39] = face.shade;
    return data;
}

function perState(value, index)
{
    return Array.isArray(value) ? value[index] : value;
}

for (const shape of behaviour.shapes)
{
    shapes.push([boxes.length, shape.length]);
    for (const box of shape)
    {
        boxes.push(box.map(v => Math.round(v * 256)));
    }
}
const count = 1 + Math.max(...Object.values(registry).map(b => Math.max(...b.states.map(s => s.id))));
const states = Buffer.alloc(count * 16);
let group = 0;
for (const [name, block] of Object.entries(registry))
{
    ++group;
    const short = name.replace(/^minecraft:/, '');
    const definition = readJson(path.join(base, 'blockstates', short + '.json'));
    const info = behaviour.blocks[name];
    for (const state of block.states)
    {
        const properties = state.properties || {};
        const index = state.id - info.first_state;
        const fluid = perState(info.fluid, index);
        const air = ['air', 'cave_air', 'void_air', 'structure_void', 'light', 'barrier'].includes(short);
        const pureFluid = short === 'water' || short === 'lava';
        const water = fluid && fluid.fluid === 'minecraft:water';
        const transparent = /glass|ice$|slime_block|honey_block/.test(short);
        const entries = [];
        for (const [key, value] of Object.entries(definition.variants || {}))
        {
            const condition = Object.fromEntries(key.split(',').filter(Boolean).map(p => p.split('=')));
            if (matches(condition, properties))
            {
                entries.push(select(value));
                break;
            }
        }
        for (const part of definition.multipart || [])
        {
            if (matches(part.when, properties))
            {
                entries.push(select(part.apply));
            }
        }
        let tint = /leaves|vine|grass|fern|lily_pad|sugar_cane|bush|moss|stem/.test(short) ? 1 : 0;
        if (short === 'redstone_wire')
        {
            tint = 2;
        }
        let geometry = air || pureFluid ? [] : entries.flatMap(e => compile(e, tint));
        if (pureFluid)
        {
            geometry = directions.map((_, side) => ({ points: vertices(side, [0,0,0], [16,16,16]), coords: [[0,16],[0,0],[16,0],[16,16]], normal: normals[side], cull: side, tile: texture('block/' + short + '_still', {}), tint: water ? 3 : 0, shade: 0 }));
        }
        if (!air && !pureFluid && !geometry.length && perState(info.collision, index))
        {
            const fallback = /chest/.test(short) ? 'block/oak_planks' : 'block/stone';
            geometry = directions.map((_, side) => ({ points: vertices(side, [0,0,0], [16,16,16]), coords: [[0,16],[0,0],[16,0],[16,16]], normal: normals[side], cull: side, tile: texture(fallback, {}), tint: 0, shade: 0 }));
        }
        if (water && !pureFluid)
        {
            // Waterlogged models keep opaque geometry and add a separate fluid layer.
            geometry.push(...directions.map((_, side) => ({ points: vertices(side, [0,0,0], [16,16,16]), coords: [[0,16],[0,0],[16,0],[16,16]], normal: normals[side], cull: side, tile: texture('block/water_still', {}), tint: 3, shade: 0 })));
        }
        const ids = geometry.map(f =>
        {
            const data = packedFace(f);
            const key = data.toString('base64');
            if (!faceIds.has(key))
            {
                faceIds.set(key, faces.length);
                faces.push(data);
            }
            return faceIds.get(key);
        });
        const key = ids.join(',');
        if (!modelRanges.has(key))
        {
            modelRanges.set(key, [references.length, ids.length]);
            references.push(...ids);
        }
        let mask = 0;
        let fullMask = 0;
        if (!pureFluid)
        {
            for (const f of geometry)
            {
                if (f.tint === 3)
                {
                    continue;
                }
                const side = sideOf(f.normal);
                if (side < 0)
                {
                    continue;
                }
                const full = vertices(side, [0,0,0], [16,16,16]);
                if (f.points.every(p => full.some(q => q.every((v, i) => Math.abs(v-p[i]) < 0.001))))
                {
                    fullMask |= 1 << side;
                    if (opacity[textureNames[f.tile]])
                    {
                        mask |= 1 << side;
                    }
                }
            }
        }
        const [start, length] = modelRanges.get(key);
        const at = state.id * 16;
        states.writeUInt32LE(start, at);
        states.writeUInt16LE(length, at + 4);
        states.writeUInt16LE(perState(info.collision, index) || 0, at + 6);
        const sameCull = fullMask === 63;
        states[at + 8] = (air ? 1 : 0) | (pureFluid ? 2 : 0) | (transparent || short === 'water' ? 4 : 0) | (water ? 8 : 0) | (sameCull ? 16 : 0);
        states[at + 9] = transparent || /leaves/.test(short) ? 0 : mask;
        states[at + 10] = fluid ? Math.round(fluid.height * 255) : 0;
        states[at + 11] = pureFluid ? (water ? 1 : 2) : 0;
        states.writeUInt16LE(perState(info.outline, index) || 0, at + 12);
        states.writeUInt16LE(group, at + 14);
    }
}
if (textureNames.length > 4096 || faces.length * 40 + references.length * 4 + states.length > 8 * 1024 * 1024)
{
    throw new Error('Compiled models exceed PSP resource budget');
}
const header = Buffer.alloc(28);
header.write('CCB3');
[count, faces.length, references.length, shapes.length, boxes.length, Math.ceil(textureNames.length / 1024)].forEach((v, i) => header.writeUInt32LE(v, 4 + i * 4));
const refs = Buffer.alloc(references.length * 4);
references.forEach((v, i) => refs.writeUInt32LE(v, i * 4));
const shapeData = Buffer.alloc(shapes.length * 8);
shapes.forEach(([start, length], i) =>
{
    shapeData.writeUInt32LE(start, i * 8);
    shapeData.writeUInt32LE(length, i * 8 + 4);
});
const boxData = Buffer.alloc(boxes.length * 12);
boxes.flat().forEach((v, i) => boxData.writeInt16LE(v, i * 2));
fs.writeFileSync(path.join(out, 'blocks.bin'), Buffer.concat([header, states, ...faces, refs, shapeData, boxData]));
fs.writeFileSync(path.join(out, 'texture-names.json'), JSON.stringify(textureNames));
console.log(`Compiled ${count} states, ${faces.length} shared faces, ${references.length} references, ${textureNames.length} textures.`);
