#pragma once

// MeowHUD World: картинки (логотипы) в мире CS2.
//
// Плоскость с текстурой рисуется в том же месте кадра, что и путь пролётки HLAE:
// после мира, до интерфейса, той же матрицей вида-проекции этого кадра и с проверкой
// по буферу глубины игры (только чтение). Поэтому стены, ящики и игроки закрывают
// картинку честно, кадр в кадр, без отставания оверлея.
//
// Вершины переводятся в clip-пространство на CPU (картинок единицы), деление на w
// делает GPU — отсечение у ближней плоскости и текстура без искажений в перспективе.
//
// Консоль: mirv_meow_world (см. Console()).

#include <array>
#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#include <d3d11.h>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CMeowWorld {
public:
	// Поток движка: вид кадра готов (может вызываться несколько раз за кадр).
	void OnEngineThread_SetupViewDone(const SOURCESDK::VMatrix& worldToScreen);
	void OnEngineThread_EndFrame();

	// Поток рендера.
	void BeginDevice(ID3D11Device* device);
	void EndDevice();
	void OnRenderThread_Draw(ID3D11DeviceContext* immediate, const D3D11_VIEWPORT* viewPort, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv);
	void OnRenderThread_Present();

	void Console(int argc, const char* const* argv);

private:
	struct Image {
		unsigned int w = 0, h = 0;
		std::vector<unsigned char> rgba; // premultiplied alpha
	};

	struct Item {
		std::string file;
		std::shared_ptr<Image> image;
		unsigned int version = 0;
		float pos[3] = {0, 0, 0};
		float ang[3] = {0, 0, 0}; // pitch yaw roll, картинка смотрит по вектору forward
		float width = 64;
		bool visible = true;
		float opacity = 1.0f;
		float brightness = 1.0f;
		int blend = 0; // 0 обычное, 1 умножение, 2 экран, 3 добавление, 4 свет стены
		bool freecamOnly = false; // только на пролётке (камера далеко от глаз игрока, за которым смотрим)
		int group = 0;    // плашки одной группы не налезают друг на друга на экране: дальние поднимаются
		float lift = 0;   // текущий подъём (плавно идёт к нужному)
		bool occlude = true; // дым закрывает картинку (выкл — у таймеров смоков над своим дымом)
		int face = 0; // 0 по углам, 1 к камере стоя (только поворот), 2 к камере с наклоном (без крена)
	};

	struct DrawQuad {
		std::string id;
		std::shared_ptr<Image> image;
		unsigned int version;
		float clip[4][4]; // TL TR BR BL, clip-пространство
		float world[4][3]; // те же углы в мире — шейдер считает, сколько луч к пикселю идёт сквозь дым
		float opacity, brightness;
		int blend;
		bool occlude;
		float dist;        // до камеры: дальние рисуем первыми, ближние поверх
	};

	struct Snapshot {
		std::vector<DrawQuad> quads;
		double m[4][4]; // вид-проекция кадра
		bool pick = false;
		float cam[3] = {0, 0, 0};
		std::vector<std::array<float, 4>> smokes; // x y z масштаб (0..1)
		float smokeShape[4] = {150, 85, 70, 18};  // радиус по горизонтали, по высоте, центр над точкой, юнитов дыма на 1/e
	};

	// Установка на поверхность под центром экрана (по глубине игры)
	struct PendingPick {
		std::string id, file;
		std::shared_ptr<Image> image;
		float width = 96;
		double camFwd[3] = {1, 0, 0};
	};
	std::unique_ptr<PendingPick> m_Pick; // под m_Mutex
	bool m_PickInFlight = false;          // под m_Mutex
	// Пролётка: камера далеко от глаз игрока, за которым смотрим (их шлёт клиент по GSI).
	// Пролётки ИИ/кампас ведёт HLAE поверх обычного режима зрителя — игра этого не видит.
	bool m_HaveObserved = false;
	double m_Observed[3] = {0, 0, 0};
	bool m_FreeCam = false;
	bool m_FreeCamReported = false;
	int m_FreeCamFrames = 0;
	double m_FrameMinDist = 1e30;
	std::string m_PendingMessage;         // под m_Mutex, печатается из потока движка

	struct GpuTex {
		unsigned int version = 0;
		ID3D11Texture2D* tex = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
	};

	std::mutex m_Mutex; // m_Items, m_Enabled, m_LastCam
	std::map<std::string, Item> m_Items;
	bool m_Enabled = true;
	unsigned int m_NextVersion = 1;
	// Смоки на карте (шлёт клиент по GSI): дым не пишет глубину, поэтому картинки за ним гасим сами
	std::vector<std::array<float, 4>> m_Smokes;
	float m_SmokeShape[4] = {150, 85, 70, 18};
	bool m_HaveCam = false;
	double m_CamPos[3] = {0, 0, 0};
	double m_CamFwd[3] = {1, 0, 0};

	std::mutex m_QueueMutex;
	std::deque<std::unique_ptr<Snapshot>> m_Queue; // nullptr = конец кадра движка

	// Поток рендера
	ID3D11Device* m_Device = nullptr;
	ID3D11DeviceContext* m_Ctx = nullptr; // deferred
	bool m_ShadersTried = false;
	ID3D11VertexShader* m_Vs = nullptr;
	ID3D11PixelShader* m_Ps = nullptr;
	ID3D11InputLayout* m_Layout = nullptr;
	ID3D11Buffer* m_Vb = nullptr;
	ID3D11Buffer* m_PsCb = nullptr;
	ID3D11SamplerState* m_Sampler = nullptr;
	ID3D11BlendState* m_Blend[5] = {};
	ID3D11DepthStencilState* m_Depth = nullptr;
	ID3D11RasterizerState* m_Raster = nullptr;
	std::map<std::string, GpuTex> m_Tex;

	bool EnsureDeviceObjects();
	void ReleaseDeviceObjects();
	GpuTex* EnsureTexture(const DrawQuad& q);
	void DoPick(ID3D11DeviceContext* immediate, const D3D11_VIEWPORT* vp, ID3D11DepthStencilView* dsv, const double m[4][4]);

	static bool LoadImageFile(const std::string& utf8Path, Image& out, std::string& error);
	static void Corners(const Item& item, float out[4][3]);
};

extern CMeowWorld g_MeowWorld;
