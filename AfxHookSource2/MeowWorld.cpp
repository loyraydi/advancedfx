#include "stdafx.h"

#include "MeowWorld.h"

#include "../shared/AfxConsole.h"

#include <d3dcompiler.h>
#include <wincodec.h>
#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

CMeowWorld g_MeowWorld;

namespace {

const double kPi = 3.14159265358979323846;

// Векторы по углам как в Source (AngleVectors): forward, right, up.
void AngleVectors(const float ang[3], double f[3], double r[3], double u[3]) {
	double sp = sin(ang[0] * kPi / 180), cp = cos(ang[0] * kPi / 180);
	double sy = sin(ang[1] * kPi / 180), cy = cos(ang[1] * kPi / 180);
	double sr = sin(ang[2] * kPi / 180), cr = cos(ang[2] * kPi / 180);
	f[0] = cp * cy; f[1] = cp * sy; f[2] = -sp;
	r[0] = -sr * sp * cy + cr * sy; r[1] = -sr * sp * sy - cr * cy; r[2] = -sr * cp;
	u[0] = cr * sp * cy + sr * sy; u[1] = cr * sp * sy - sr * cy; u[2] = cr * cp;
}

std::wstring Utf8ToWide(const std::string& s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

// clip = M * [x y z 1] (строка матрицы — компонента clip), как шейдеры HLAE.
void ToClip(const SOURCESDK::VMatrix& m, const float p[3], float out[4]) {
	for (int r = 0; r < 4; ++r)
		out[r] = (float)(m.m[r][0] * p[0] + m.m[r][1] * p[1] + m.m[r][2] * p[2] + m.m[r][3]);
}

// Положение камеры: точка, где clip x = y = w = 0. Направление взгляда — градиент w.
bool CameraFromMatrix(const SOURCESDK::VMatrix& m, double pos[3], double fwd[3]) {
	const int rows[3] = {0, 1, 3};
	double a[3][3], b[3];
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) a[i][j] = m.m[rows[i]][j];
		b[i] = -m.m[rows[i]][3];
	}
	double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
		- a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
		+ a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	if (fabs(det) < 1e-12) return false;
	for (int k = 0; k < 3; ++k) {
		double t[3][3];
		memcpy(t, a, sizeof(t));
		for (int i = 0; i < 3; ++i) t[i][k] = b[i];
		double dk = t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1])
			- t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0])
			+ t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
		pos[k] = dk / det;
	}
	double len = sqrt(m.m[3][0] * m.m[3][0] + m.m[3][1] * m.m[3][1] + m.m[3][2] * m.m[3][2]);
	if (len < 1e-12) return false;
	for (int i = 0; i < 3; ++i) fwd[i] = m.m[3][i] / len;
	return true;
}

const char* kShader = R"(
cbuffer Style : register(b0) {
	float4 cStyle;      // x opacity, y brightness, z 1 = «свет стены», w 1 = дым закрывает
	float4 cCam;        // xyz камера, w число смоков
	float4 cShape;      // x радиус по горизонтали, y по высоте, z центр над точкой, w юнитов дыма на 1/e
	float4 cSmoke[16];  // xyz точка смока, w масштаб 0..1 (растёт/тает)
};
Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);
struct VsIn { float4 clip : POSITION; float2 uv : TEXCOORD0; float3 world : TEXCOORD1; };
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float3 world : TEXCOORD1; };
VsOut vs_main(VsIn i) { VsOut o; o.pos = i.clip; o.uv = i.uv; o.world = i.world; return o; }
// Сколько света проходит от пикселя к камере сквозь облака дыма (эллипсоиды)
float SmokeTransmittance(float3 p) {
	float3 c = cCam.xyz;
	float3 seg = p - c;
	float segLen = length(seg);
	float t = 1.0f;
	int n = (int)cCam.w;
	[loop] for (int k = 0; k < n; ++k) {
		float s = cSmoke[k].w;
		if (s <= 0.01f) continue;
		float3 r = float3(cShape.x, cShape.x, cShape.y) * s;
		float3 ctr = cSmoke[k].xyz + float3(0, 0, cShape.z);
		float3 o = (c - ctr) / r, d = seg / r;
		float a = dot(d, d), b = 2.0f * dot(o, d), cc = dot(o, o) - 1.0f;
		float disc = b * b - 4.0f * a * cc;
		if (disc <= 0.0f || a < 1e-8f) continue;
		float sq = sqrt(disc);
		float t0 = saturate((-b - sq) / (2.0f * a)), t1 = saturate((-b + sq) / (2.0f * a));
		float inside = (t1 - t0) * segLen;
		t *= exp(-inside / max(cShape.w, 1.0f));
	}
	return t;
}
float4 ps_main(VsOut i) : SV_TARGET {
	float4 c = tex0.Sample(smp0, i.uv); // premultiplied
	c.rgb *= cStyle.y;
	c *= cStyle.x;
	if (cStyle.w > 0.5f && cCam.w > 0.5f) c *= SmokeTransmittance(i.world);
	clip(c.a - 0.002f);
	if (cStyle.z > 0.5f) {
		// «Свет стены»: множитель для цвета стены (blend ZERO, SRC_COLOR):
		// стена × (1 − a + 2·цвет) — серый 50% не меняет стену, белый светлит, чёрный темнит.
		// Тени, свет и фактура стены остаются — белый логотип тоже их берёт.
		return float4(1.0f - c.a + 2.0f * c.rgb, 1.0f);
	}
	return c;
}
)";

struct Vtx { float clip[4]; float uv[2]; float world[3]; };
struct PsConsts { float style[4]; float cam[4]; float shape[4]; float smoke[16][4]; };

bool Invert4(const double a[4][4], double inv[4][4]) {
	double m[4][8];
	for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) m[i][j] = j < 4 ? a[i][j] : (j - 4 == i ? 1 : 0);
	for (int c = 0; c < 4; ++c) {
		int piv = c;
		for (int r = c + 1; r < 4; ++r) if (fabs(m[r][c]) > fabs(m[piv][c])) piv = r;
		if (fabs(m[piv][c]) < 1e-15) return false;
		if (piv != c) for (int j = 0; j < 8; ++j) { double t = m[c][j]; m[c][j] = m[piv][j]; m[piv][j] = t; }
		double d = m[c][c];
		for (int j = 0; j < 8; ++j) m[c][j] /= d;
		for (int r = 0; r < 4; ++r) if (r != c) { double f = m[r][c]; for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j]; }
	}
	for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) inv[i][j] = m[i][j + 4];
	return true;
}

// Значение глубины из texel по формату буфера глубины.
bool DepthFromTexel(DXGI_FORMAT f, const unsigned char* p, double& out) {
	switch (f) {
	case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: {
		float v; memcpy(&v, p, 4); out = v; return true;
	}
	case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: {
		unsigned int v; memcpy(&v, p, 4); out = (v & 0xFFFFFF) / 16777215.0; return true;
	}
	case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: {
		unsigned short v; memcpy(&v, p, 2); out = v / 65535.0; return true;
	}
	default: return false;
	}
}

unsigned int TexelBytes(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return 8;
	case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: return 2;
	default: return 4;
	}
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

} // namespace

// ---------------------------------------------------------------------------
// Поток движка

void CMeowWorld::Corners(const Item& item, float out[4][3]) {
	double f[3], r[3], u[3];
	AngleVectors(item.ang, f, r, u);
	// Зритель смотрит навстречу forward, его «право» — это -right рамки.
	double rv[3] = {-r[0], -r[1], -r[2]};
	double w = item.width / 2.0;
	double h = item.image && item.image->w ? item.width * item.image->h / (double)item.image->w / 2.0 : w;
	const double sx[4] = {-1, 1, 1, -1}, sy[4] = {1, 1, -1, -1}; // TL TR BR BL
	for (int k = 0; k < 4; ++k)
		for (int i = 0; i < 3; ++i)
			out[k][i] = (float)(item.pos[i] + rv[i] * w * sx[k] + u[i] * h * sy[k]);
}

void CMeowWorld::OnEngineThread_SetupViewDone(const SOURCESDK::VMatrix& m) {
	std::unique_ptr<Snapshot> snap(new Snapshot());
	for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) snap->m[r][c] = m.m[r][c];
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		m_HaveCam = CameraFromMatrix(m, m_CamPos, m_CamFwd);
		if (!m_PendingMessage.empty()) { advancedfx::Message("%s", m_PendingMessage.c_str()); m_PendingMessage.clear(); }
		snap->pick = m_Pick != nullptr;
		for (int i = 0; i < 3; ++i) snap->cam[i] = (float)m_CamPos[i];
		snap->smokes = m_Smokes;
		for (int i = 0; i < 4; ++i) snap->smokeShape[i] = m_SmokeShape[i];
		if (!m_Enabled && !snap->pick) return;
		struct Prep { const std::string* id; Item* it; float c[4][3]; double dist; };
		std::vector<Prep> prep;
		for (auto& kv : m_Items) {
			Item& it = kv.second;
			if (!m_Enabled || !it.visible || !it.image || !it.image->w) continue;
			Prep p;
			p.id = &kv.first;
			p.it = &it;
			float (&c)[4][3] = p.c;
			if (it.face && m_HaveCam) {
				// Лицом к камере: forward смотрит из картинки на камеру, крена нет — стоит ровно
				Item faced = it;
				double n[3] = {m_CamPos[0] - it.pos[0], m_CamPos[1] - it.pos[1], m_CamPos[2] - it.pos[2]};
				double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (len > 0.001) {
					for (int i = 0; i < 3; ++i) n[i] /= len;
					faced.ang[1] = (float)(atan2(n[1], n[0]) * 180 / kPi);
					faced.ang[0] = it.face == 2 ? (float)(-asin(n[2] < -1 ? -1 : n[2] > 1 ? 1 : n[2]) * 180 / kPi) : 0.0f;
					faced.ang[2] = 0;
				}
				Corners(faced, c);
			} else Corners(it, c);
			double d[3] = {it.pos[0] - m_CamPos[0], it.pos[1] - m_CamPos[1], it.pos[2] - m_CamPos[2]};
			p.dist = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			prep.push_back(p);
		}

		// Плашки одной группы (над игроками) не налезают друг на друга НА ЭКРАНЕ: ближняя к камере
		// стоит на месте, дальняя поднимается этажом выше, пока не перестанет перекрываться.
		// Сравниваем верхние 3/4 картинки (низ — тонкая ножка). Подъём идёт плавно.
		std::sort(prep.begin(), prep.end(), [](const Prep& a, const Prep& b) { return a.dist < b.dist; });
		struct Rect { int group; float x0, y0, x1, y1; };
		std::vector<Rect> rects;
		auto screenRect = [&](const float c[4][3], float lift, Rect& r) {
			float pts[4][3];
			for (int k = 0; k < 4; ++k) for (int i = 0; i < 3; ++i) pts[k][i] = c[k][i] + (i == 2 ? lift : 0.0f);
			// низ без ножки: 3/4 высоты от верха
			for (int k = 2; k < 4; ++k) for (int i = 0; i < 3; ++i) pts[k][i] = pts[3 - k][i] + (pts[k][i] - pts[3 - k][i]) * 0.75f;
			r.x0 = r.y0 = 1e9f; r.x1 = r.y1 = -1e9f;
			for (int k = 0; k < 4; ++k) {
				float cl[4];
				ToClip(m, pts[k], cl);
				if (cl[3] <= 1.0f) return false; // за камерой
				float x = cl[0] / cl[3], y = cl[1] / cl[3];
				r.x0 = x < r.x0 ? x : r.x0; r.x1 = x > r.x1 ? x : r.x1;
				r.y0 = y < r.y0 ? y : r.y0; r.y1 = y > r.y1 ? y : r.y1;
			}
			return true;
		};
		for (Prep& p : prep) {
			Item& it = *p.it;
			if (!it.group || !m_HaveCam) continue;
			double hx = p.c[0][0] - p.c[3][0], hy = p.c[0][1] - p.c[3][1], hz = p.c[0][2] - p.c[3][2];
			float step = (float)(sqrt(hx * hx + hy * hy + hz * hz) * 0.8);
			float target = 0;
			Rect placed = {it.group, 0, 0, 0, 0};
			bool visible = false;
			for (int lvl = 0; lvl <= 4; ++lvl) {
				Rect r = {it.group, 0, 0, 0, 0};
				if (!screenRect(p.c, lvl * step, r)) break;
				bool clash = false;
				for (const Rect& o : rects)
					if (o.group == it.group && r.x0 < o.x1 + 0.004f && o.x0 < r.x1 + 0.004f && r.y0 < o.y1 + 0.006f && o.y0 < r.y1 + 0.006f) { clash = true; break; }
				target = lvl * step; placed = r; visible = true;
				if (!clash) break;
			}
			if (visible) rects.push_back(placed);
			it.lift += (target - it.lift) * 0.18f;
			if (fabs(target - it.lift) < 0.05f) it.lift = target;
			for (int k = 0; k < 4; ++k) p.c[k][2] += it.lift;
		}

		for (Prep& p : prep) {
			const Item& it = *p.it;
			DrawQuad q;
			q.id = *p.id;
			q.image = it.image;
			q.version = it.version;
			for (int k = 0; k < 4; ++k) { ToClip(m, p.c[k], q.clip[k]); for (int i = 0; i < 3; ++i) q.world[k][i] = p.c[k][i]; }
			q.occlude = it.occlude;
			q.opacity = it.opacity;
			q.brightness = it.brightness;
			q.blend = it.blend;
			q.dist = (float)p.dist;
			snap->quads.push_back(std::move(q));
		}
		// Дальние первыми, ближние поверх: картинки не пишут глубину, порядок решает, кто сверху
		std::reverse(snap->quads.begin(), snap->quads.end());
	}
	if (snap->quads.empty() && !snap->pick) return;

	// Как у пути пролётки: за кадр остаётся последний вид (после скайбокса и после мира).
	std::unique_lock<std::mutex> lock(m_QueueMutex);
	if (!m_Queue.empty() && m_Queue.back()) m_Queue.pop_back();
	m_Queue.emplace_back(std::move(snap));
}

void CMeowWorld::OnEngineThread_EndFrame() {
	std::unique_lock<std::mutex> lock(m_QueueMutex);
	m_Queue.emplace_back(nullptr);
}

// ---------------------------------------------------------------------------
// Поток рендера

void CMeowWorld::BeginDevice(ID3D11Device* device) {
	EndDevice();
	if (!device) return;
	device->AddRef();
	m_Device = device;
	m_Device->CreateDeferredContext(0, &m_Ctx);
}

void CMeowWorld::EndDevice() {
	ReleaseDeviceObjects();
	SafeRelease(m_Ctx);
	SafeRelease(m_Device);
}

void CMeowWorld::ReleaseDeviceObjects() {
	for (auto& kv : m_Tex) { SafeRelease(kv.second.srv); SafeRelease(kv.second.tex); }
	m_Tex.clear();
	SafeRelease(m_Vs); SafeRelease(m_Ps); SafeRelease(m_Layout); SafeRelease(m_Vb); SafeRelease(m_PsCb);
	SafeRelease(m_Sampler); SafeRelease(m_Depth); SafeRelease(m_Raster);
	for (auto& b : m_Blend) SafeRelease(b);
	m_ShadersTried = false;
}

bool CMeowWorld::EnsureDeviceObjects() {
	if (m_Vs && m_Ps) return true;
	if (m_ShadersTried || !m_Device) return false;
	m_ShadersTried = true;

	ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
	if (FAILED(D3DCompile(kShader, strlen(kShader), "meow_world", nullptr, nullptr, "vs_main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &err))) {
		advancedfx::Warning("mirv_meow_world: vertex shader: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
		SafeRelease(err);
		return false;
	}
	if (FAILED(D3DCompile(kShader, strlen(kShader), "meow_world", nullptr, nullptr, "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &err))) {
		advancedfx::Warning("mirv_meow_world: pixel shader: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
		SafeRelease(err); SafeRelease(vs);
		return false;
	}
	bool ok = SUCCEEDED(m_Device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_Vs))
		&& SUCCEEDED(m_Device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_Ps));
	D3D11_INPUT_ELEMENT_DESC layout[3] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	ok = ok && SUCCEEDED(m_Device->CreateInputLayout(layout, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &m_Layout));
	SafeRelease(vs); SafeRelease(ps);

	D3D11_BUFFER_DESC vb = {};
	vb.ByteWidth = sizeof(Vtx) * 6;
	vb.Usage = D3D11_USAGE_DYNAMIC;
	vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ok = ok && SUCCEEDED(m_Device->CreateBuffer(&vb, nullptr, &m_Vb));

	D3D11_BUFFER_DESC cb = {};
	cb.ByteWidth = sizeof(PsConsts);
	cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ok = ok && SUCCEEDED(m_Device->CreateBuffer(&cb, nullptr, &m_PsCb));

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_ANISOTROPIC;
	sd.MaxAnisotropy = 8;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	ok = ok && SUCCEEDED(m_Device->CreateSamplerState(&sd, &m_Sampler));

	// Глубина игры только на чтение: картинку закрывает всё, что ближе к камере.
	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = TRUE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	ok = ok && SUCCEEDED(m_Device->CreateDepthStencilState(&dd, &m_Depth));

	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	rd.SlopeScaledDepthBias = -1.0f; // лежит на поверхности — чуть ближе, чтобы не мерцало
	ok = ok && SUCCEEDED(m_Device->CreateRasterizerState(&rd, &m_Raster));

	// premultiplied: обычное, умножение («как краска»), экран, добавление
	// 4 — «свет стены»: шейдер отдаёт множитель, стена × множитель
	const D3D11_BLEND src[5] = {D3D11_BLEND_ONE, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO};
	const D3D11_BLEND dst[5] = {D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_SRC_COLOR};
	for (int i = 0; i < 5; ++i) {
		D3D11_BLEND_DESC bd = {};
		auto& t = bd.RenderTarget[0];
		t.BlendEnable = TRUE;
		t.SrcBlend = src[i];
		t.DestBlend = dst[i];
		t.BlendOp = D3D11_BLEND_OP_ADD;
		t.SrcBlendAlpha = D3D11_BLEND_ZERO;
		t.DestBlendAlpha = D3D11_BLEND_ONE;
		t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		ok = ok && SUCCEEDED(m_Device->CreateBlendState(&bd, &m_Blend[i]));
	}

	if (!ok) {
		advancedfx::Warning("mirv_meow_world: failed to create device objects.\n");
		ReleaseDeviceObjects();
		m_ShadersTried = true;
	}
	return ok;
}

CMeowWorld::GpuTex* CMeowWorld::EnsureTexture(const DrawQuad& q) {
	GpuTex& g = m_Tex[q.id];
	if (g.srv && g.version == q.version) return &g;
	SafeRelease(g.srv); SafeRelease(g.tex);
	g.version = q.version;

	const Image& im = *q.image;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = im.w;
	td.Height = im.h;
	td.MipLevels = 0; // полная цепочка, строим GenerateMips
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	if (FAILED(m_Device->CreateTexture2D(&td, nullptr, &g.tex))) return nullptr;
	if (FAILED(m_Device->CreateShaderResourceView(g.tex, nullptr, &g.srv))) { SafeRelease(g.tex); return nullptr; }
	m_Ctx->UpdateSubresource(g.tex, 0, nullptr, im.rgba.data(), im.w * 4, 0);
	m_Ctx->GenerateMips(g.srv);
	return &g;
}

void CMeowWorld::OnRenderThread_Draw(ID3D11DeviceContext* immediate, const D3D11_VIEWPORT* viewPort, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) {
	std::unique_ptr<Snapshot> snap;
	{
		std::unique_lock<std::mutex> lock(m_QueueMutex);
		if (!m_Queue.empty() && m_Queue.front()) {
			snap = std::move(m_Queue.front());
			m_Queue.pop_front();
		}
	}
	if (!snap || !immediate || !rtv || !viewPort || !m_Ctx) return;
	if (snap->pick) DoPick(immediate, viewPort, dsv, snap->m);
	if (snap->quads.empty()) return;
	if (!EnsureDeviceObjects()) return;

	m_Ctx->OMSetRenderTargets(1, &rtv, dsv);
	m_Ctx->RSSetViewports(1, viewPort);
	m_Ctx->RSSetState(m_Raster);
	m_Ctx->OMSetDepthStencilState(m_Depth, 0);
	m_Ctx->IASetInputLayout(m_Layout);
	m_Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	UINT stride = sizeof(Vtx), offset = 0;
	m_Ctx->IASetVertexBuffers(0, 1, &m_Vb, &stride, &offset);
	m_Ctx->VSSetShader(m_Vs, nullptr, 0);
	m_Ctx->PSSetShader(m_Ps, nullptr, 0);
	m_Ctx->PSSetSamplers(0, 1, &m_Sampler);
	m_Ctx->PSSetConstantBuffers(0, 1, &m_PsCb);

	const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	const int tri[6] = {0, 1, 2, 0, 2, 3};
	for (const DrawQuad& q : snap->quads) {
		GpuTex* g = EnsureTexture(q);
		if (!g) continue;

		D3D11_MAPPED_SUBRESOURCE ms;
		if (FAILED(m_Ctx->Map(m_Vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
		Vtx* v = (Vtx*)ms.pData;
		for (int i = 0; i < 6; ++i) {
			memcpy(v[i].clip, q.clip[tri[i]], sizeof(v[i].clip));
			memcpy(v[i].world, q.world[tri[i]], sizeof(v[i].world));
			v[i].uv[0] = uv[tri[i]][0];
			v[i].uv[1] = uv[tri[i]][1];
		}
		m_Ctx->Unmap(m_Vb, 0);

		if (SUCCEEDED(m_Ctx->Map(m_PsCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			PsConsts pc = {};
			pc.style[0] = q.opacity; pc.style[1] = q.brightness; pc.style[2] = q.blend == 4 ? 1.0f : 0.0f; pc.style[3] = q.occlude ? 1.0f : 0.0f;
			size_t n = snap->smokes.size() < 16 ? snap->smokes.size() : 16;
			for (int i = 0; i < 3; ++i) pc.cam[i] = snap->cam[i];
			pc.cam[3] = (float)n;
			for (int i = 0; i < 4; ++i) pc.shape[i] = snap->smokeShape[i];
			for (size_t k = 0; k < n; ++k) for (int i = 0; i < 4; ++i) pc.smoke[k][i] = snap->smokes[k][i];
			memcpy(ms.pData, &pc, sizeof(pc));
			m_Ctx->Unmap(m_PsCb, 0);
		}

		int b = q.blend < 0 || q.blend > 4 ? 0 : q.blend;
		m_Ctx->OMSetBlendState(m_Blend[b], nullptr, 0xffffffff);
		m_Ctx->PSSetShaderResources(0, 1, &g->srv);
		m_Ctx->Draw(6, 0);
	}

	ID3D11CommandList* list = nullptr;
	m_Ctx->FinishCommandList(FALSE, &list);
	if (list) {
		immediate->ExecuteCommandList(list, TRUE); // состояние игры восстанавливается
		list->Release();
	}
}


void CMeowWorld::DoPick(ID3D11DeviceContext* immediate, const D3D11_VIEWPORT* vp, ID3D11DepthStencilView* dsv, const double m[4][4]) {
	std::unique_ptr<PendingPick> pick;
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		pick = std::move(m_Pick);
	}
	if (!pick) return;
	auto fail = [&](const char* why) {
		std::unique_lock<std::mutex> lock(m_Mutex);
		m_PendingMessage = std::string("mirv_meow_world wall: ") + why + "\n";
	};
	if (!dsv) return fail("no depth buffer this frame");

	ID3D11Resource* res = nullptr;
	dsv->GetResource(&res);
	ID3D11Texture2D* depth = nullptr;
	if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&depth))) { SafeRelease(res); return fail("depth is not a 2D texture"); }
	SafeRelease(res);
	D3D11_TEXTURE2D_DESC td;
	depth->GetDesc(&td);
	if (td.SampleDesc.Count > 1) { SafeRelease(depth); return fail("depth is multisampled (MSAA) - turn MSAA off to place pictures"); }
	D3D11_DEPTH_STENCIL_VIEW_DESC dd;
	dsv->GetDesc(&dd);

	D3D11_TEXTURE2D_DESC sd = td;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	ID3D11Texture2D* staging = nullptr;
	if (FAILED(m_Device->CreateTexture2D(&sd, nullptr, &staging))) { SafeRelease(depth); return fail("cannot create staging texture"); }
	immediate->CopyResource(staging, depth);
	SafeRelease(depth);

	// центр экрана и две соседние точки — для нормали поверхности
	const int cx = (int)(vp->TopLeftX + vp->Width / 2), cy = (int)(vp->TopLeftY + vp->Height / 2);
	const int px[3][2] = {{cx, cy}, {cx + 6, cy}, {cx, cy + 6}};
	double world[3][3];
	bool ok = true;
	D3D11_MAPPED_SUBRESOURCE ms;
	UINT sub = dd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY ? D3D11CalcSubresource(dd.Texture2DArray.MipSlice, dd.Texture2DArray.FirstArraySlice, td.MipLevels) : 0;
	if (FAILED(immediate->Map(staging, sub, D3D11_MAP_READ, 0, &ms))) { SafeRelease(staging); return fail("cannot read depth"); }
	double inv[4][4];
	if (!Invert4(m, inv)) ok = false;
	for (int k = 0; ok && k < 3; ++k) {
		int x = px[k][0], y = px[k][1];
		if (x < 0 || y < 0 || x >= (int)td.Width || y >= (int)td.Height) { ok = false; break; }
		double d;
		if (!DepthFromTexel(td.Format, (const unsigned char*)ms.pData + (size_t)y * ms.RowPitch + (size_t)x * TexelBytes(td.Format), d)) { ok = false; break; }
		double ndc[4] = {
			(x + 0.5 - vp->TopLeftX) / vp->Width * 2 - 1,
			1 - (y + 0.5 - vp->TopLeftY) / vp->Height * 2,
			vp->MaxDepth > vp->MinDepth ? (d - vp->MinDepth) / (vp->MaxDepth - vp->MinDepth) : d,
			1};
		double w4[4];
		for (int r = 0; r < 4; ++r) w4[r] = inv[r][0] * ndc[0] + inv[r][1] * ndc[1] + inv[r][2] * ndc[2] + inv[r][3] * ndc[3];
		if (fabs(w4[3]) < 1e-12) { ok = false; break; }
		for (int i = 0; i < 3; ++i) world[k][i] = w4[i] / w4[3];
	}
	immediate->Unmap(staging, sub);
	SafeRelease(staging);
	if (!ok) {
		char why[128];
		snprintf(why, sizeof(why), "could not read the surface under the crosshair (depth format %d)", (int)td.Format);
		return fail(why);
	}

	double a[3] = {world[1][0] - world[0][0], world[1][1] - world[0][1], world[1][2] - world[0][2]};
	double b[3] = {world[2][0] - world[0][0], world[2][1] - world[0][1], world[2][2] - world[0][2]};
	double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
	double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len < 1e-9) return fail("surface under the crosshair is too far to measure");
	for (double& v : n) v /= len;
	if (n[0] * pick->camFwd[0] + n[1] * pick->camFwd[1] + n[2] * pick->camFwd[2] > 0) for (double& v : n) v = -v;

	Item it;
	it.file = pick->file;
	it.image = pick->image;
	for (int i = 0; i < 3; ++i) it.pos[i] = (float)(world[0][i] + n[i] * 0.5);
	if (fabs(n[2]) > 0.92) {
		// пол или потолок: верх картинки — от камеры
		it.ang[0] = n[2] > 0 ? -90.0f : 90.0f;
		double yaw = atan2(pick->camFwd[1], pick->camFwd[0]) * 180 / kPi;
		it.ang[1] = (float)(n[2] > 0 ? yaw + 180 : yaw);
		it.ang[2] = 0;
	} else {
		it.ang[0] = (float)(-asin(n[2]) * 180 / kPi);
		it.ang[1] = (float)(atan2(n[1], n[0]) * 180 / kPi);
		it.ang[2] = 0;
	}
	it.width = pick->width;
	it.visible = true;

	std::unique_lock<std::mutex> lock(m_Mutex);
	it.version = m_NextVersion++;
	auto old = m_Items.find(pick->id);
	if (old != m_Items.end()) { it.opacity = old->second.opacity; it.brightness = old->second.brightness; it.blend = old->second.blend; }
	m_Items[pick->id] = it;
	char buf[512];
	snprintf(buf, sizeof(buf), "mirv_meow_world set %s \"%s\" %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n", pick->id.c_str(), pick->file.c_str(),
		it.pos[0], it.pos[1], it.pos[2], it.ang[0], it.ang[1], it.ang[2], it.width);
	m_PendingMessage = buf;
}

void CMeowWorld::OnRenderThread_Present() {
	std::unique_lock<std::mutex> lock(m_QueueMutex);
	while (!m_Queue.empty()) {
		bool marker = !m_Queue.front();
		m_Queue.pop_front();
		if (marker && m_Queue.size() <= 128) break;
	}
}

// ---------------------------------------------------------------------------
// Картинки

bool CMeowWorld::LoadImageFile(const std::string& utf8Path, Image& out, std::string& error) {
	HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	IWICImagingFactory* factory = nullptr;
	IWICBitmapDecoder* decoder = nullptr;
	IWICBitmapFrameDecode* frame = nullptr;
	IWICFormatConverter* conv = nullptr;
	bool ok = false;
	do {
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) { error = "WIC unavailable"; break; }
		std::wstring w = Utf8ToWide(utf8Path);
		if (FAILED(factory->CreateDecoderFromFilename(w.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) { error = "cannot open image"; break; }
		if (FAILED(decoder->GetFrame(0, &frame))) { error = "no frame"; break; }
		if (FAILED(factory->CreateFormatConverter(&conv))) { error = "no converter"; break; }
		if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) { error = "convert failed"; break; }
		UINT w0 = 0, h0 = 0;
		conv->GetSize(&w0, &h0);
		if (!w0 || !h0 || w0 > 8192 || h0 > 8192) { error = "bad size"; break; }
		out.w = w0; out.h = h0;
		out.rgba.resize((size_t)w0 * h0 * 4);
		if (FAILED(conv->CopyPixels(nullptr, w0 * 4, (UINT)out.rgba.size(), out.rgba.data()))) { error = "read failed"; break; }
		ok = true;
	} while (false);
	SafeRelease(conv); SafeRelease(frame); SafeRelease(decoder); SafeRelease(factory);
	if (SUCCEEDED(hrCo)) CoUninitialize();
	return ok;
}

// ---------------------------------------------------------------------------
// Консоль

void CMeowWorld::Console(int argc, const char* const* argv) {
	const char* cmd = argc >= 2 ? argv[1] : "";
	auto f = [&](int i, float d) { return argc > i ? (float)atof(argv[i]) : d; };

	if (!_stricmp(cmd, "set") && argc >= 11) {
		// set <id> <file> <x> <y> <z> <pitch> <yaw> <roll> <width> [visible]
		auto img = std::make_shared<Image>();
		std::string error;
		if (!LoadImageFile(argv[3], *img, error)) {
			advancedfx::Warning("mirv_meow_world: %s: %s\n", argv[3], error.c_str());
			return;
		}
		std::unique_lock<std::mutex> lock(m_Mutex);
		Item& it = m_Items[argv[2]];
		it.file = argv[3];
		it.image = img;
		it.version = m_NextVersion++;
		it.pos[0] = f(4, 0); it.pos[1] = f(5, 0); it.pos[2] = f(6, 0);
		it.ang[0] = f(7, 0); it.ang[1] = f(8, 0); it.ang[2] = f(9, 0);
		it.width = f(10, 64);
		it.visible = argc > 11 ? atoi(argv[11]) != 0 : true;
		it.face = argc > 12 ? atoi(argv[12]) : 0;
		return;
	}
	if (!_stricmp(cmd, "here") && argc >= 4) {
		// here <id> <file> [width] [distance] — перед камерой, лицом к ней
		auto img = std::make_shared<Image>();
		std::string error;
		if (!LoadImageFile(argv[3], *img, error)) {
			advancedfx::Warning("mirv_meow_world: %s: %s\n", argv[3], error.c_str());
			return;
		}
		std::unique_lock<std::mutex> lock(m_Mutex);
		if (!m_HaveCam) { advancedfx::Warning("mirv_meow_world: no camera yet.\n"); return; }
		float dist = f(5, 128);
		Item& it = m_Items[argv[2]];
		it.file = argv[3];
		it.image = img;
		it.version = m_NextVersion++;
		for (int i = 0; i < 3; ++i) it.pos[i] = (float)(m_CamPos[i] + m_CamFwd[i] * dist);
		// картинка смотрит на камеру: forward = -взгляд
		double n[3] = {-m_CamFwd[0], -m_CamFwd[1], -m_CamFwd[2]};
		it.ang[0] = (float)(-asin(n[2] < -1 ? -1 : n[2] > 1 ? 1 : n[2]) * 180 / kPi);
		it.ang[1] = (float)(atan2(n[1], n[0]) * 180 / kPi);
		it.ang[2] = 0;
		it.width = f(4, 64);
		it.visible = true;
		advancedfx::Message("mirv_meow_world set %s \"%s\" %.1f %.1f %.1f %.1f %.1f 0 %.1f\n", argv[2], argv[3], it.pos[0], it.pos[1], it.pos[2], it.ang[0], it.ang[1], it.width);
		return;
	}
	if (!_stricmp(cmd, "wall") && argc >= 4) {
		// wall <id> <file> [width] — на стену/пол под прицелом (центр экрана)
		auto img = std::make_shared<Image>();
		std::string error;
		if (!LoadImageFile(argv[3], *img, error)) {
			advancedfx::Warning("mirv_meow_world: %s: %s\n", argv[3], error.c_str());
			return;
		}
		std::unique_lock<std::mutex> lock(m_Mutex);
		std::unique_ptr<PendingPick> p(new PendingPick());
		p->id = argv[2];
		p->file = argv[3];
		p->image = img;
		p->width = f(4, 96);
		if (m_HaveCam) for (int i = 0; i < 3; ++i) p->camFwd[i] = m_CamFwd[i];
		m_Pick = std::move(p);
		return;
	}
	if (!_stricmp(cmd, "move") && argc >= 10) {
		std::unique_lock<std::mutex> lock(m_Mutex);
		auto it = m_Items.find(argv[2]);
		if (it == m_Items.end()) return;
		it->second.pos[0] = f(3, 0); it->second.pos[1] = f(4, 0); it->second.pos[2] = f(5, 0);
		it->second.ang[0] = f(6, 0); it->second.ang[1] = f(7, 0); it->second.ang[2] = f(8, 0);
		it->second.width = f(9, it->second.width);
		return;
	}
	if (!_stricmp(cmd, "style") && argc >= 4) {
		// style <id> <opacity 0..1> [blend 0..3] [brightness 0..2]
		std::unique_lock<std::mutex> lock(m_Mutex);
		auto it = m_Items.find(argv[2]);
		if (it == m_Items.end()) return;
		it->second.opacity = f(3, 1);
		it->second.blend = argc > 4 ? atoi(argv[4]) : it->second.blend;
		it->second.brightness = f(5, it->second.brightness);
		return;
	}
	if (!_stricmp(cmd, "smokes")) {
		// smokes [x y z масштаб]... — все смоки на карте разом (пусто — смоков нет)
		std::unique_lock<std::mutex> lock(m_Mutex);
		m_Smokes.clear();
		for (int i = 2; i + 3 < argc && m_Smokes.size() < 16; i += 4)
			m_Smokes.push_back({(float)atof(argv[i]), (float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3])});
		return;
	}
	if (!_stricmp(cmd, "smokeshape") && argc >= 6) {
		// smokeshape <радиус> <высота> <центр над точкой> <юнитов на 1/e>
		std::unique_lock<std::mutex> lock(m_Mutex);
		for (int i = 0; i < 4; ++i) m_SmokeShape[i] = (float)atof(argv[2 + i]);
		return;
	}
	if (!_stricmp(cmd, "group") && argc >= 4) {
		// group <id> <n> — картинки одной группы (n > 0) разводятся на экране, дальние поднимаются
		std::unique_lock<std::mutex> lock(m_Mutex);
		auto it = m_Items.find(argv[2]);
		if (it != m_Items.end()) it->second.group = atoi(argv[3]);
		return;
	}
	if (!_stricmp(cmd, "occlude") && argc >= 4) {
		std::unique_lock<std::mutex> lock(m_Mutex);
		auto it = m_Items.find(argv[2]);
		if (it != m_Items.end()) it->second.occlude = atoi(argv[3]) != 0;
		return;
	}
	if (!_stricmp(cmd, "face") && argc >= 4) {
		// face <id> 0|1|2 — 1 к камере стоя, 2 к камере с наклоном
		std::unique_lock<std::mutex> lock(m_Mutex);
		auto it = m_Items.find(argv[2]);
		if (it != m_Items.end()) it->second.face = atoi(argv[3]);
		return;
	}
	if (!_stricmp(cmd, "visible") && argc >= 4) {
		std::unique_lock<std::mutex> lock(m_Mutex);
		auto it = m_Items.find(argv[2]);
		if (it != m_Items.end()) it->second.visible = atoi(argv[3]) != 0;
		return;
	}
	if (!_stricmp(cmd, "remove") && argc >= 3) {
		std::unique_lock<std::mutex> lock(m_Mutex);
		m_Items.erase(argv[2]);
		return;
	}
	if (!_stricmp(cmd, "clear")) {
		std::unique_lock<std::mutex> lock(m_Mutex);
		m_Items.clear();
		return;
	}
	if (!_stricmp(cmd, "enabled")) {
		std::unique_lock<std::mutex> lock(m_Mutex);
		if (argc >= 3) m_Enabled = atoi(argv[2]) != 0;
		advancedfx::Message("mirv_meow_world enabled %i\n", m_Enabled ? 1 : 0);
		return;
	}
	if (!_stricmp(cmd, "list")) {
		std::unique_lock<std::mutex> lock(m_Mutex);
		for (auto& kv : m_Items) {
			const Item& it = kv.second;
			advancedfx::Message("%s \"%s\" %.1f %.1f %.1f | %.1f %.1f %.1f | w %.1f | %s\n", kv.first.c_str(), it.file.c_str(),
				it.pos[0], it.pos[1], it.pos[2], it.ang[0], it.ang[1], it.ang[2], it.width, it.visible ? "on" : "off");
		}
		if (m_HaveCam) advancedfx::Message("camera %.1f %.1f %.1f\n", m_CamPos[0], m_CamPos[1], m_CamPos[2]);
		return;
	}
	advancedfx::Message(
		"mirv_meow_world set <id> <file.png> <x> <y> <z> <pitch> <yaw> <roll> <width> [visible] [face 0|1|2] - picture in the world, faces (pitch, yaw); floor = pitch -90\n"
		"mirv_meow_world wall <id> <file.png> [width=96] - onto the wall/floor under the crosshair (screen centre)\n"
		"mirv_meow_world here <id> <file.png> [width=64] [distance=128] - in front of the camera, facing it\n"
		"mirv_meow_world move <id> <x> <y> <z> <pitch> <yaw> <roll> <width>\n"
		"mirv_meow_world style <id> <opacity 0..1> [blend 0 normal|1 multiply|2 screen|3 add|4 wall light] [brightness 0..2]\n"
		"mirv_meow_world face <id> 0|1|2 - 0 fixed angles, 1 face the camera upright, 2 face the camera with pitch (no roll)\n"
		"mirv_meow_world smokes [<x> <y> <z> <scale 0..1>]... - smokes on the map (pictures behind them fade out); no args = none\n"
		"mirv_meow_world smokeshape <radius=150> <height=85> <centre above point=70> <units per 1/e=18>\n"
		"mirv_meow_world group <id> <n> - pictures of one group (n > 0) never overlap on screen: farther ones rise\n"
		"mirv_meow_world occlude <id> 0|1 - smokes hide this picture (default 1)\n"
		"mirv_meow_world visible <id> 0|1 | remove <id> | clear | enabled [0|1] | list\n"
	);
}
