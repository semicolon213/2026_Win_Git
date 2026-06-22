#pragma once

#include <string>
#include <vector>

// 오피스 문서(pptx/docx 등 ZIP+XML 구조)에서 텍스트를 추출
// 내부적으로 miniz로 압축을 풀어 각 포맷의 텍스트 태그만 모음
// miniz는 이 모듈(OfficeText.cpp) 안에서만 사용하여 다른 코드와 충돌 회피
// 실패하거나 대상이 아니면 빈 vector 반환 (호출 측에서 조용히 생략)

// pptx: 슬라이드 텍스트를 줄 단위로 추출. outSlideCount가 nullptr이 아니면 장 수도 반환(빈 장 포함, 실패 시 0)
std::vector<std::wstring> ExtractPptxText(const std::wstring& fullPath, unsigned long long sizeBytes, int* outSlideCount = nullptr);
bool IsPptxFileName(const std::wstring& name);

// docx: 본문 텍스트를 줄(문단) 단위로 추출 (<w:t> 태그).
std::vector<std::wstring> ExtractDocxText(const std::wstring& fullPath, unsigned long long sizeBytes);
bool IsDocxFileName(const std::wstring& name);