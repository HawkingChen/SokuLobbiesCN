#pragma once

#include <string>

namespace RatingRequests {
enum class State { Loading, NoRecord, Dns, Connect, Timeout, Tls, Certificate, CaFile, Http, Transfer, InvalidResponse, Internal, InvalidName };
struct Status { State state = State::Loading; long http = 0; };

inline std::wstring text(Status status, bool chinese)
{
	switch (status.state) {
	case State::Loading: return chinese ? L"\u6B63\u5728\u52A0\u8F7D\u8BC4\u5206" : L"Loading rating...";
	case State::NoRecord: return chinese ? L"\u672A\u627E\u5230\u8BC4\u5206\u8BB0\u5F55" : L"No rating record found";
	case State::Dns: return chinese ? L"\u8BC4\u5206\u8BF7\u6C42\u5931\u8D25\uFF1A\u57DF\u540D\u89E3\u6790\u5931\u8D25" : L"Rating request: DNS failed";
	case State::Connect: return chinese ? L"\u8BC4\u5206\u8BF7\u6C42\u5931\u8D25\uFF1A\u65E0\u6CD5\u8FDE\u63A5\u670D\u52A1\u5668" : L"Rating request: cannot connect";
	case State::Timeout: return chinese ? L"\u8BC4\u5206\u8BF7\u6C42\u5931\u8D25\uFF1A\u8FDE\u63A5\u6216\u8BF7\u6C42\u8D85\u65F6" : L"Rating request: timed out";
	case State::Tls: return chinese ? L"\u8BC4\u5206\u8BF7\u6C42\u5931\u8D25\uFF1ATLS \u63E1\u624B\u5931\u8D25" : L"Rating request: TLS handshake failed";
	case State::Certificate: return chinese ? L"\u8BC4\u5206\u8BF7\u6C42\u5931\u8D25\uFF1A\u8BC1\u4E66\u9A8C\u8BC1\u5931\u8D25" : L"Rating request: certificate rejected";
	case State::CaFile: return chinese ? L"\u8BC4\u5206\u8BF7\u6C42\u5931\u8D25\uFF1A\u8BC1\u4E66\u6587\u4EF6\u7F3A\u5931\u6216\u65E0\u6CD5\u8BFB\u53D6" : L"Rating request: CA file unreadable";
	case State::Http: return (chinese ? L"\u8BC4\u5206\u670D\u52A1\u8FD4\u56DE\u9519\u8BEF\uFF1AHTTP " : L"Rating service: HTTP ") + std::to_wstring(status.http);
	case State::Transfer: return chinese ? L"\u8BC4\u5206\u8BF7\u6C42\u5931\u8D25\uFF1A\u6570\u636E\u4F20\u8F93\u4E2D\u65AD" : L"Rating request: transfer failed";
	case State::InvalidResponse: return chinese ? L"\u8BC4\u5206\u670D\u52A1\u8FD4\u56DE\u6570\u636E\u5F02\u5E38" : L"Invalid rating service response";
	case State::InvalidName: return chinese ? L"\u73A9\u5BB6\u540D\u7F16\u7801\u65E0\u6548\uFF0C\u65E0\u6CD5\u67E5\u8BE2\u8BC4\u5206" : L"Invalid player name encoding";
	default: return chinese ? L"\u8BC4\u5206\u52A0\u8F7D\u5668\u5F02\u5E38" : L"Rating loader error";
	}
}
}
