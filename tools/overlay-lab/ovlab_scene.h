// ovlab_scene.h -- the lab game's picture: one pixel shader, nothing loaded from disk.
//
// A walk along a dirt path through rolling grass, with rocks, bushes and brick columns beside
// it, under a sky with clouds, lit by a low sun with shadows. Everything is procedural (value
// noise and signed distances, ray-marched), so it is the same picture for the same frame
// number on any machine, and it has what a relighting model reacts to: materials with
// texture, soft and hard edges, shadowed and lit sides, depth and haze. A HUD is drawn on top.

#pragma once

static const char *kShader = R"hlsl(
cbuffer C : register(b0) { float4 T; };   // time, width, height, unused
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2); o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }

float h1(float2 p) { float3 q = frac(float3(p.xyx) * 0.1031); q += dot(q, q.yzx + 33.33); return frac((q.x + q.y) * q.z); }
float vnoise(float2 p)
{
    float2 i = floor(p), f = frac(p); f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(h1(i), h1(i + float2(1, 0)), f.x), lerp(h1(i + float2(0, 1)), h1(i + float2(1, 1)), f.x), f.y);
}
float fbm(float2 p, int oct)
{
    float a = 0.5, s = 0.0;
    [loop] for (int i = 0; i < oct; ++i) { s += a * vnoise(p); p = p * 2.03 + 17.3; a *= 0.5; }
    return s;
}

float pathx(float z) { return 2.5 * sin(z * 0.08) + 1.2 * sin(z * 0.031 + 1.0); }
float ground(float2 p, int oct)
{
    float h = 2.6 * fbm(p * 0.06, oct) - 1.3;
    float d = abs(p.x - pathx(p.y));
    return lerp(h * 0.3 - 0.2, h, smoothstep(0.8, 4.5, d));   // the path runs in a shallow, flatter trough
}
float sdBox(float3 p, float3 b) { float3 q = abs(p) - b; return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0); }

)hlsl"
// (MSVC takes at most 16 KB in one string literal; adjacent literals are joined.)
R"hlsl(
// ---- The companion: a person standing close in front of the camera, facing it. A neural
// relighting model does its most visible work on faces, so the lab has one: skull and jaw,
// nose, brow, lips, ears, eye sockets with eyeballs, hair, neck, shoulders, arms, a tunic.
// Placed from the camera each frame (set in ps), so it is always there, at arm's length.
static float3 gNpcPos;    // feet, world
static float3 gNpcAx;     // its right, world (horizontal)
static float3 gNpcAz;     // the way it faces, world (horizontal)
static float  gNpcTurn;   // head turn, radians

float smin(float a, float b, float k) { float h = saturate(0.5 + 0.5 * (b - a) / k); return lerp(b, a, h) - k * h * (1.0 - h); }
float smax(float a, float b, float k) { return -smin(-a, -b, k); }
float sdEll(float3 p, float3 r) { float k0 = length(p / r), k1 = length(p / (r * r)); return k0 * (k0 - 1.0) / k1; }
float sdCap(float3 p, float3 a, float3 b, float r) { float3 pa = p - a, ba = b - a; return length(pa - ba * saturate(dot(pa, ba) / dot(ba, ba))) - r; }

float3 npcBody(float3 pw) { float3 d = pw - gNpcPos; return float3(dot(d, gNpcAx), d.y, dot(d, gNpcAz)); }
// Head space: origin at the top of the neck (1.445 m up), +z out of the face, +y up.
float3 npcHead(float3 pw)
{
    float3 h = npcBody(pw) - float3(0.0, 1.445, 0.0);
    float cs = cos(gNpcTurn), sn = sin(gNpcTurn);
    return float3(cs * h.x - sn * h.z, h.y, sn * h.x + cs * h.z);
}

// Distance and material: 5 skin, 6 eye, 7 hair, 8 clothes.
float2 npc(float3 pw)
{
    float3 p = npcBody(pw), h = npcHead(pw);
    float3 m = float3(abs(h.x), h.y, h.z);   // the face is symmetric

    float skin = sdEll(h - float3(0.0, 0.135, 0.0), float3(0.094, 0.116, 0.104));                 // skull
    skin = smin(skin, sdEll(h - float3(0.0, 0.060, 0.020), float3(0.071, 0.084, 0.078)), 0.035);   // jaw and chin
    skin = smin(skin, sdEll(m - float3(0.046, 0.092, 0.060), float3(0.030, 0.030, 0.030)), 0.030); // cheekbones
    skin = smin(skin, sdEll(h - float3(0.0, 0.108, 0.104), float3(0.0135, 0.031, 0.020)), 0.012);  // bridge of the nose
    skin = smin(skin, length(h - float3(0.0, 0.089, 0.115)) - 0.0135, 0.008);                      // its tip
    skin = smin(skin, length(m - float3(0.012, 0.084, 0.106)) - 0.008, 0.006);                     // nostril wings
    skin = smin(skin, sdCap(h, float3(-0.050, 0.160, 0.089), float3(0.050, 0.160, 0.089), 0.010), 0.014);   // brow ridge
    skin = smin(skin, sdEll(h - float3(0.0, 0.054, 0.090), float3(0.026, 0.0075, 0.012)), 0.006);  // upper lip
    skin = smin(skin, sdEll(h - float3(0.0, 0.044, 0.088), float3(0.023, 0.0085, 0.012)), 0.006);  // lower lip
    skin = smin(skin, sdEll(m - float3(0.097, 0.125, -0.006), float3(0.011, 0.031, 0.017)), 0.008); // ears
    skin = smax(skin, -(length(m - float3(0.033, 0.139, 0.097)) - 0.0150), 0.009);                 // eye sockets
    skin = smin(skin, sdEll(m - float3(0.033, 0.1475, 0.0905), float3(0.0170, 0.0060, 0.0100)), 0.004);   // upper lids
    skin = smin(skin, sdEll(m - float3(0.033, 0.1315, 0.0895), float3(0.0160, 0.0045, 0.0090)), 0.004);   // lower lids
    skin = smin(skin, sdCap(p, float3(0.0, 1.36, -0.014), float3(0.0, 1.47, -0.006), 0.056), 0.028);   // neck

    float eye = length(m - float3(0.033, 0.139, 0.0835)) - 0.0125;

    // Hair: a cap a little larger than the skull, kept on top and at the back and sides,
    // cut away over the face and below the collar line.
    float hair = sdEll(h - float3(0.0, 0.150, -0.016), float3(0.104, 0.119, 0.118));
    hair = max(hair, min(h.z - 0.036, 0.206 - h.y));
    hair = max(hair, 0.020 - h.y);
    hair -= 0.004 * vnoise(float2(h.x * 240.0 + h.z * 130.0, h.y * 22.0));

    float body = sdEll(p - float3(0.0, 1.325, 0.0), float3(0.215, 0.090, 0.115));                                  // shoulders
    body = smin(body, sdBox(p - float3(0.0, 1.00, 0.0), float3(0.135, 0.27, 0.060)) - 0.045, 0.05);                 // chest
    body = smin(body, sdCap(float3(abs(p.x), p.y, p.z), float3(0.225, 1.29, 0.0), float3(0.250, 0.93, 0.03), 0.050), 0.03);   // arms
    body = smin(body, sdBox(p - float3(0.0, 0.40, 0.0), float3(0.125, 0.40, 0.055)) - 0.04, 0.05);                  // the tunic to the ground

    float2 r = float2(skin, 5.0);
    if (eye < r.x)  r = float2(eye, 6.0);
    if (hair < r.x) r = float2(hair, 7.0);
    if (body < r.x) r = float2(body, 8.0);
    return r;
}

// Distance and material: 1 ground, 2 rock, 3 bush, 4 column, 5-8 the companion.
float2 map(float3 p, int oct)
{
    float2 r = float2(p.y - ground(p.xz, oct), 1.0);

    float2 c = floor(p.xz / 7.0);
    float  k = h1(c + 3.7);
    float3 ctr = float3((c.x + 0.5) * 7.0 + (h1(c) - 0.5) * 3.0, 0.0, (c.y + 0.5) * 7.0 + (h1(c + 9.1) - 0.5) * 3.0);
    if (abs(ctr.x - pathx(ctr.z)) > 2.6)
    {
        float rad = 0.45 + 0.85 * h1(c + 5.5);
        if (k > 0.52)
        {
            ctr.y = ground(ctr.xz, 2) + rad * 0.25;
            float3 q = (p - ctr) / float3(1.0, 0.68, 1.25);
            float d = (length(q) - rad) * 0.68;
            d += 0.22 * (vnoise(p.xz * 2.1 + p.y * 1.7) - 0.5) + 0.06 * (vnoise(p.xz * 8.0 + p.y * 6.0) - 0.5);
            if (d < r.x) r = float2(d, 2.0);
        }
        else if (k < 0.24)
        {
            rad *= 0.75;
            ctr.y = ground(ctr.xz, 2) + rad * 0.6;
            float d = length(p - ctr) - rad + 0.30 * (vnoise(p.xz * 5.0 + p.y * 4.0) - 0.5) + 0.10 * (vnoise(p.xz * 17.0 - p.y * 13.0) - 0.5);
            d *= 0.6;
            if (d < r.x) r = float2(d, 3.0);
        }
    }

    float zc = floor(p.z / 16.0 + 0.5) * 16.0;
    [unroll] for (int s = -1; s <= 1; s += 2)
    {
        float3 b = float3(pathx(zc) + 3.4 * s, 0.0, zc);
        b.y = ground(b.xz, 2);
        float d = sdBox(p - b - float3(0, 1.9, 0), float3(0.5, 2.2, 0.5));
        d = min(d, sdBox(p - b - float3(0, 4.2, 0), float3(0.68, 0.14, 0.68)));   // a cap stone
        if (d < r.x) r = float2(d, 4.0);
    }

    // The companion, behind a bounding sphere so the rest of the scene does not pay for it.
    float db = length(p - gNpcPos - float3(0.0, 0.95, 0.0)) - 1.05;
    if (db < 0.25) { float2 n = npc(p); if (n.x < r.x) r = n; }
    else r.x = min(r.x, db);
    return r;
}

)hlsl"
// (MSVC takes at most 16 KB in one string literal; adjacent literals are joined.)
R"hlsl(
float3 normalAt(float3 p, float t)
{
    float2 e = float2(max(0.0008, 0.0010 * t), 0.0);   // fine enough for a face at arm's length
    return normalize(float3(map(p + e.xyy, 5).x - map(p - e.xyy, 5).x,
                            map(p + e.yxy, 5).x - map(p - e.yxy, 5).x,
                            map(p + e.yyx, 5).x - map(p - e.yyx, 5).x));
}

float shadow(float3 p, float3 l)
{
    float res = 1.0, t = 0.08;
    [loop] for (int i = 0; i < 22; ++i)
    {
        float d = map(p + l * t, 2).x;
        res = min(res, 9.0 * d / t);
        t += clamp(d, 0.06, 1.2);
        if (res < 0.02 || t > 26.0) break;
    }
    return saturate(res);
}

float3 sky(float3 rd, float3 sun, float t)
{
    float3 col = lerp(float3(0.60, 0.72, 0.86), float3(0.13, 0.32, 0.72), pow(saturate(rd.y), 0.50));
    if (rd.y > 0.0)
    {
        float2 uv = rd.xz / (rd.y + 0.12) * 0.9 + float2(t * 0.012, 0.0);
        float cl = smoothstep(0.52, 0.80, fbm(uv, 5));
        col = lerp(col, float3(1.0, 0.98, 0.95) * (0.75 + 0.25 * fbm(uv * 2.7 + 4.0, 3)), cl * saturate(rd.y * 5.0) * 0.85);
    }
    float s = saturate(dot(rd, sun));
    return col + float3(1.0, 0.82, 0.55) * (pow(s, 180.0) * 2.0 + pow(s, 12.0) * 0.25);
}

float box(float2 p, float2 a, float2 b) { return step(a.x, p.x) * step(p.x, b.x) * step(a.y, p.y) * step(p.y, b.y); }

)hlsl"
// (MSVC takes at most 16 KB in one string literal; adjacent literals are joined.)
R"hlsl(
float4 ps(V i) : SV_Target
{
    float t = T.x; float2 res = T.yz; float2 px = i.uv * res;
    float2 q = (px - 0.5 * res) / res.y; q.y = -q.y;

    // The camera walks the path at eye height, looking a little ahead and slowly from side to side.
    float cz = t * 1.4;
    float3 ro = float3(pathx(cz), 0.0, cz); ro.y = ground(ro.xz, 3) + 1.7;
    float3 ta = float3(pathx(cz + 7.0) + 2.2 * sin(t * 0.23), 0.0, cz + 7.0); ta.y = ground(ta.xz, 3) + 1.35;
    float3 fw = normalize(ta - ro), rt = normalize(cross(float3(0, 1, 0), fw)), up = cross(fw, rt);
    float3 rd = normalize(q.x * rt + q.y * up + 1.25 * fw);
    // The sun is behind the camera and to one side, so what faces the camera is lit.
    float3 sun = normalize(float3(0.62, 0.46, -0.52));

    // The companion: 1.12 m ahead, a little to the left (the panel opens on the right),
    // facing the camera, its head turning slowly.
    float3 fh = normalize(float3(fw.x, 0.0, fw.z)), rh = float3(fh.z, 0.0, -fh.x);
    gNpcPos = ro + fh * 1.12 - rh * 0.22; gNpcPos.y = ground(gNpcPos.xz, 3) - 0.02;
    gNpcAz = -fh; gNpcAx = float3(gNpcAz.z, 0.0, -gNpcAz.x);
    gNpcTurn = 0.30 * sin(t * 0.45);

    float dist = 0.0, mat = 0.0;
    [loop] for (int s = 0; s < 130; ++s)
    {
        float2 m = map(ro + rd * dist, 3);
        if (m.x < 0.0007 * dist + 0.0007) { mat = m.y; break; }
        dist += min(m.x * 0.72, 1.6);
        if (dist > 140.0) break;
    }

)hlsl"
// (MSVC takes at most 16 KB in one string literal; adjacent literals are joined.)
R"hlsl(
    float3 col = sky(rd, sun, t);
    if (mat > 0.5)
    {
        float3 p = ro + rd * dist, n = normalAt(p, dist), alb;
        if (mat < 1.5)
        {
            float dp = abs(p.x - pathx(p.z)) + 0.9 * (vnoise(p.xz * 1.3) - 0.5);
            float3 grass = lerp(float3(0.07, 0.17, 0.035), float3(0.27, 0.40, 0.10), fbm(p.xz * 0.9, 3));
            grass *= 0.62 + 0.75 * vnoise(float2(p.x * 41.0, p.z * 13.0));                      // blades
            grass = lerp(grass, float3(0.50, 0.44, 0.17), 0.55 * smoothstep(0.62, 0.80, vnoise(p.xz * 0.9 + 40.0)));   // dry patches
            float fl = h1(floor(p.xz * 16.0));
            if (fl > 0.986 && dist < 26.0 && length(frac(p.xz * 16.0) - 0.5) < 0.30) grass = lerp(grass, fl > 0.994 ? float3(0.95, 0.92, 0.85) : float3(0.92, 0.75, 0.12), 0.85);   // flowers
            float3 dirt = lerp(float3(0.20, 0.14, 0.09), float3(0.44, 0.34, 0.24), fbm(p.xz * 2.4, 4));
            dirt *= 0.75 + 0.5 * vnoise(p.xz * 27.0);                                           // grit
            dirt = lerp(dirt, float3(0.55, 0.53, 0.50), smoothstep(0.80, 0.86, vnoise(p.xz * 6.5)));   // pebbles
            alb = lerp(dirt, grass, smoothstep(0.85, 1.9, dp));
            n = normalize(n + 0.18 * float3(vnoise(p.xz * 31.0) - 0.5, 0.0, vnoise(p.xz * 31.0 + 9.0) - 0.5));
        }
        else if (mat < 2.5)
        {
            float f = fbm(p.xz * 2.6 + p.y * 2.1, 4);
            alb = lerp(float3(0.25, 0.24, 0.23), float3(0.56, 0.54, 0.50), f);
            alb *= 0.8 + 0.4 * vnoise(p.xz * 19.0 + p.y * 23.0);
            alb = lerp(alb, float3(0.10, 0.09, 0.09), smoothstep(0.47, 0.50, abs(vnoise(p.xz * 3.1 - p.y * 2.3) - 0.5) * 2.0 + 0.48) * 0.0);
            alb = lerp(alb, float3(0.16, 0.27, 0.07), smoothstep(0.55, 0.9, n.y) * smoothstep(0.35, 0.65, vnoise(p.xz * 3.7)));   // moss on top
        }
        else if (mat < 3.5)
        {
            alb = lerp(float3(0.05, 0.14, 0.03), float3(0.20, 0.36, 0.08), vnoise(p.xz * 9.0 + p.y * 11.0));
            alb *= 0.6 + 0.8 * vnoise(p.xz * 37.0 - p.y * 29.0);
        }
        else if (mat > 4.5)
        {
            float3 h = npcHead(p), b = npcBody(p);
            float3 hairc = lerp(float3(0.07, 0.045, 0.03), float3(0.26, 0.17, 0.10), vnoise(float2(h.x * 260.0 + h.z * 140.0, h.y * 16.0)));
            if (mat < 5.5)
            {
                alb = float3(0.60, 0.41, 0.32);
                alb *= 0.93 + 0.14 * vnoise(h.xy * 700.0 + h.z * 300.0);                                              // pores
                float front = smoothstep(0.02, 0.05, h.z);
                alb = lerp(alb, float3(0.66, 0.33, 0.28), 0.38 * front * smoothstep(0.050, 0.008, length(float2(abs(h.x) - 0.050, h.y - 0.088))));   // cheeks
                alb = lerp(alb, float3(0.64, 0.36, 0.30), 0.30 * front * smoothstep(0.022, 0.004, length(float2(h.x, h.y - 0.092))));               // nose
                float lips = smoothstep(0.0125, 0.0070, abs(h.y - 0.049)) * smoothstep(0.029, 0.021, abs(h.x)) * smoothstep(0.070, 0.082, h.z);
                alb = lerp(alb, float3(0.52, 0.20, 0.20), 0.85 * lips);
                alb *= 1.0 - 0.65 * smoothstep(0.0022, 0.0006, abs(h.y - 0.0492)) * smoothstep(0.025, 0.018, abs(h.x)) * smoothstep(0.075, 0.085, h.z);   // the line of the mouth
                float brow = smoothstep(0.0085, 0.0045, abs(h.y - 0.1635 + 6.0 * (abs(h.x) - 0.032) * (abs(h.x) - 0.032)))
                           * smoothstep(0.009, 0.015, abs(h.x)) * smoothstep(0.060, 0.052, abs(h.x)) * smoothstep(0.068, 0.080, h.z);
                alb = lerp(alb, hairc * 0.7, 0.9 * brow);
                alb *= 1.0 - 0.30 * smoothstep(0.022, 0.013, length(float2(abs(h.x) - 0.033, h.y - 0.139))) * smoothstep(0.070, 0.085, h.z);   // shade in the sockets
                alb *= 1.0 - 0.55 * smoothstep(0.0016, 0.0004, abs(h.y - 0.1438)) * smoothstep(0.017, 0.012, abs(abs(h.x) - 0.033)) * smoothstep(0.075, 0.085, h.z);   // lash line
                alb = lerp(alb, alb * float3(0.78, 0.80, 0.84), 0.5 * front * smoothstep(0.085, 0.035, h.y) * vnoise(h.xy * 1500.0));           // a day's stubble
            }
            else if (mat < 6.5)
            {
                float3 v = h - float3(sign(h.x) * 0.033, 0.139, 0.0835);
                float rr = length(v.xy + float2(0.0, 0.0005));
                alb = float3(0.70, 0.67, 0.64) * (0.80 + 0.20 * smoothstep(0.012, 0.006, rr));
                if (v.z > 0.0)
                {
                    float iris = smoothstep(0.0058, 0.0051, rr);
                    float3 ic = lerp(float3(0.10, 0.20, 0.32), float3(0.30, 0.48, 0.58), vnoise(float2(atan2(v.y, v.x) * 7.0, rr * 1400.0)));
                    ic *= smoothstep(0.0059, 0.0042, rr) * 0.6 + 0.4;                                      // a darker rim
                    alb = lerp(alb, ic, iris);
                    alb = lerp(alb, float3(0.01, 0.01, 0.012), smoothstep(0.0025, 0.0020, rr));           // pupil
                }
            }
            else if (mat < 7.5) alb = hairc;
            else
            {
                alb = lerp(float3(0.10, 0.17, 0.27), float3(0.17, 0.27, 0.40), vnoise(b.xy * 55.0 + b.z * 31.0));
                alb *= 0.82 + 0.36 * vnoise(float2(b.x * 420.0, b.y * 420.0));                             // weave
                alb = lerp(alb, float3(0.30, 0.18, 0.09), smoothstep(0.035, 0.025, abs(b.y - 0.92)));     // a belt
                alb = lerp(alb, float3(0.50, 0.42, 0.27), smoothstep(0.012, 0.006, abs(length(float2(b.x, b.y - 1.40)) - 0.085)) * smoothstep(0.03, 0.06, b.z));   // collar trim
            }
        }
        else
        {
            float row = floor(p.y * 3.2);
            float2 uv = float2((p.x + p.z) * 2.4 + 0.5 * fmod(abs(row), 2.0), p.y * 3.2);
            float2 f = frac(uv);
            float mortar = saturate(step(f.x, 0.07) + step(f.y, 0.13));
            float3 brick = lerp(float3(0.50, 0.27, 0.19), float3(0.66, 0.42, 0.30), h1(floor(uv) + 2.0));
            brick *= 0.8 + 0.4 * vnoise(uv * 9.0);
            alb = lerp(brick, float3(0.62, 0.60, 0.56), mortar);
            alb = lerp(alb, float3(0.20, 0.24, 0.12), 0.5 * smoothstep(0.4, 0.0, p.y - ground(p.xz, 2)) * vnoise(p.xz * 5.0 + p.y * 3.0));   // damp at the foot
        }

        float sh = shadow(p + n * 0.03, sun);
        float dif = saturate(dot(n, sun)) * sh;
        float3 lit = alb * (float3(1.30, 1.12, 0.88) * dif * 1.9
                          + float3(0.30, 0.42, 0.62) * (0.20 + 0.26 * n.y)
                          + float3(0.20, 0.17, 0.10) * saturate(-n.y * 0.5 + 0.2));
        float3 hv = normalize(sun - rd);
        lit += (mat > 1.5 && mat < 2.5 ? 0.18 : 0.05) * pow(saturate(dot(n, hv)), 28.0) * sh;
        if (mat > 4.5 && mat < 5.5)
        {
            // Skin: light wraps round a little and comes back warm, and it has a soft sheen.
            lit += alb * float3(0.55, 0.20, 0.12) * saturate((dot(n, sun) + 0.45) / 1.45) * (0.35 + 0.65 * sh) * 0.55;
            lit += 0.10 * pow(saturate(dot(n, hv)), 14.0) * sh;
        }
        else if (mat > 5.5 && mat < 6.5) lit += 1.4 * pow(saturate(dot(n, hv)), 220.0) + 0.08;   // a wet eye
        else if (mat > 6.5 && mat < 7.5) lit += 0.22 * pow(saturate(dot(n, hv)), 40.0) * sh;    // hair
        float fog = 1.0 - exp(-dist * 0.0065);
        col = lerp(lit, sky(normalize(float3(rd.x, 0.03, rd.z)), sun, t) * 0.92, fog);
    }
    col = 1.0 - exp(-col * 1.15);

    // HUD: health and stamina bars, a compass strip, a crosshair.
    float2 u = px;
    float hud = 0.0; float3 hc = 0;
    float hp = 0.72 + 0.1 * sin(t * 0.7);
    if (box(u, float2(28, res.y - 54), float2(28 + 260, res.y - 34)) > 0) { hud = 0.85; hc = u.x < 28 + 260 * hp ? float3(0.80, 0.16, 0.14) : float3(0.10, 0.10, 0.10); }
    if (box(u, float2(28, res.y - 28), float2(28 + 200, res.y - 18)) > 0) { hud = 0.85; hc = u.x < 28 + 200 * 0.55 ? float3(0.20, 0.62, 0.86) : float3(0.10, 0.10, 0.10); }
    if (box(u, float2(res.x * 0.5 - 180, 18), float2(res.x * 0.5 + 180, 34)) > 0)
    { hud = 0.7; float tick = step(0.9, frac((u.x + fw.x * 400.0) / 30.0)); hc = lerp(float3(0.05, 0.05, 0.06), float3(0.9, 0.9, 0.9), tick); }
    float2 cr = abs(u - 0.5 * res);
    if ((cr.x < 1.0 && cr.y < 9.0 && cr.y > 3.0) || (cr.y < 1.0 && cr.x < 9.0 && cr.x > 3.0)) { hud = 0.9; hc = float3(1, 1, 1); }
    col = lerp(col, hc, hud);
    return float4(pow(saturate(col), 1.0 / 2.2), 1.0);
}
)hlsl";
