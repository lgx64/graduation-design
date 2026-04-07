const fileInput = document.getElementById('source-file');
const projectZipInput = document.getElementById('project-zip');
const filenameInput = document.getElementById('filename-input');
const editor = document.getElementById('source-editor');
const analyzeButton = document.getElementById('analyze-btn');
const loadSampleButton = document.getElementById('load-sample-btn');
const diagnosticList = document.getElementById('diagnostic-list');
const bugSummary = document.getElementById('bug-summary');
const codeViewer = document.getElementById('code-viewer');
const statusText = document.getElementById('status-text');
const rawReportText = document.getElementById('raw-report-text');
const rawOutputText = document.getElementById('raw-output-text');
const fileList = document.getElementById('file-list');
const fileSummary = document.getElementById('file-summary');
const passFilters = document.getElementById('pass-filters');
const activeFileLabel = document.getElementById('active-file-label');
const modePill = document.getElementById('mode-pill');
const statBugs = document.getElementById('stat-bugs');
const statFiles = document.getElementById('stat-files');
const statFailed = document.getElementById('stat-failed');
const statTime = document.getElementById('stat-time');

const state = {
    payload: null,
    selectedFilePath: null,
    selectedLine: null,
    passFilters: new Set(),
};

function escapeHtml(text) {
    return String(text || '')
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
}

function setStatus(mode, text) {
    statusText.className = `status ${mode}`;
    statusText.textContent = text;
}

function getSelectedFile() {
    const files = state.payload?.files || [];
    return files.find(file => file.path === state.selectedFilePath) || null;
}

function getVisibleDiagnostics() {
    const file = getSelectedFile();
    if (!file) {
        return [];
    }
    const filters = state.passFilters;
    if (!filters.size) {
        return file.diagnostics || [];
    }
    return (file.diagnostics || []).filter(diag => filters.has(diag.pass_name));
}

function lineMessageMap(diagnostics) {
    const result = new Map();
    for (const diag of diagnostics) {
        const items = result.get(diag.line) || [];
        items.push(diag);
        result.set(diag.line, items);
    }
    return result;
}

function renderCode(source, diagnostics, activeLine = null) {
    const byLine = lineMessageMap(diagnostics);
    const lines = String(source || '').split('\n');
    const html = lines.map((line, index) => {
        const lineNo = index + 1;
        const items = byLine.get(lineNo) || [];
        const hasBug = items.length > 0;
        const marker = hasBug
            ? `<span class="line-marker">${escapeHtml(items.map(item => item.pass_name).join(' / '))}</span>`
            : '';
        const activeClass = activeLine === lineNo ? ' active' : '';
        const bugClass = hasBug ? ' has-bug' : '';
        return `
            <div class="code-line${bugClass}${activeClass}" data-line="${lineNo}">
                <div class="line-number">${lineNo}</div>
                <div class="line-content">${escapeHtml(line || ' ')}${marker}</div>
            </div>
        `;
    }).join('');
    codeViewer.innerHTML = html;
}

function scrollToLine(lineNo) {
    const lineNode = codeViewer.querySelector(`.code-line[data-line="${lineNo}"]`);
    if (!lineNode) {
        return;
    }
    codeViewer.querySelectorAll('.code-line.active').forEach(node => node.classList.remove('active'));
    lineNode.classList.add('active');
    lineNode.scrollIntoView({ behavior: 'smooth', block: 'center' });
}

function renderStats(payload) {
    const summary = payload?.summary || {};
    statBugs.textContent = summary.bug_count ?? 0;
    statFiles.textContent = summary.file_count ?? 0;
    statFailed.textContent = summary.failed_files ?? 0;
    statTime.textContent = summary.generated_at ? summary.generated_at.replace('T', ' ') : '-';
    modePill.textContent = payload?.mode === 'project' ? '工程 zip' : '单文件';
}

function renderFilters(payload) {
    const passCounts = payload?.summary?.pass_counts || {};
    const names = Object.keys(passCounts);
    if (!names.length) {
        passFilters.className = 'pass-filters empty';
        passFilters.innerHTML = '暂无类型';
        state.passFilters.clear();
        return;
    }

    passFilters.className = 'pass-filters';
    passFilters.innerHTML = names.map(name => {
        const active = state.passFilters.size === 0 || state.passFilters.has(name);
        return `<button type="button" class="filter-chip${active ? ' active' : ''}" data-pass="${escapeHtml(name)}">${escapeHtml(name)} (${passCounts[name]})</button>`;
    }).join('');

    passFilters.querySelectorAll('.filter-chip').forEach(node => {
        node.addEventListener('click', () => {
            const passName = node.dataset.pass;
            if (state.passFilters.has(passName)) {
                state.passFilters.delete(passName);
            } else {
                state.passFilters.add(passName);
            }
            if (state.passFilters.size === Object.keys(passCounts).length) {
                state.passFilters.clear();
            }
            refreshActiveFileView();
            renderFilters(state.payload);
        });
    });
}

function renderFiles(payload) {
    const files = payload?.files || [];
    fileSummary.textContent = `${files.length} 个文件`;
    if (!files.length) {
        fileList.className = 'file-list empty';
        fileList.innerHTML = '尚未上传工程';
        return;
    }

    fileList.className = 'file-list';
    fileList.innerHTML = files.map(file => {
        const active = file.path === state.selectedFilePath ? ' active' : '';
        const statusClass = file.status === 'ok' ? 'ok' : 'error';
        return `
            <article class="file-item${active}" data-path="${escapeHtml(file.path)}">
                <div class="file-title">${escapeHtml(file.path)}</div>
                <div class="file-meta"><span class="file-status ${statusClass}">${escapeHtml(file.status)}</span> ${file.bug_count} 个问题</div>
            </article>
        `;
    }).join('');

    fileList.querySelectorAll('.file-item').forEach(node => {
        node.addEventListener('click', () => {
            state.selectedFilePath = node.dataset.path;
            state.selectedLine = null;
            refreshActiveFileView();
        });
    });
}

function renderDiagnostics(diagnostics) {
    bugSummary.textContent = `${diagnostics.length} 个问题`;
    if (!diagnostics.length) {
        diagnosticList.className = 'diagnostic-list empty';
        diagnosticList.innerHTML = '当前文件在筛选条件下没有问题。';
        return;
    }

    diagnosticList.className = 'diagnostic-list';
    diagnosticList.innerHTML = diagnostics.map((diag, index) => `
        <article class="diagnostic-item" data-line="${diag.line}" data-index="${index}">
            <div class="diag-title"><span class="badge">${escapeHtml(diag.pass_name)}</span>${escapeHtml(diag.message)}</div>
            <div class="diag-meta">文件：${escapeHtml(diag.file_path || diag.source_path)} · 函数：${escapeHtml(diag.function)} · 行号：${diag.line}</div>
            <div class="diag-inst">IR：${escapeHtml(diag.inst)}</div>
        </article>
    `).join('');

    diagnosticList.querySelectorAll('.diagnostic-item').forEach(node => {
        node.addEventListener('click', () => {
            diagnosticList.querySelectorAll('.diagnostic-item.active').forEach(item => item.classList.remove('active'));
            node.classList.add('active');
            state.selectedLine = Number(node.dataset.line);
            scrollToLine(state.selectedLine);
        });
    });
}

function refreshActiveFileView() {
    const payload = state.payload;
    const file = getSelectedFile();
    renderFiles(payload);

    if (!file) {
        activeFileLabel.textContent = '当前文件：未选择';
        renderDiagnostics([]);
        renderCode('', []);
        rawReportText.textContent = '';
        rawOutputText.textContent = '';
        return;
    }

    const diagnostics = getVisibleDiagnostics();
    activeFileLabel.textContent = `当前文件：${file.path}`;
    rawReportText.textContent = file.report_text || '';
    rawOutputText.textContent = [file.compile_output, file.run_output, file.error].filter(Boolean).join('\n\n');
    renderDiagnostics(diagnostics);
    renderCode(file.source || '', diagnostics, state.selectedLine);

    if (!state.selectedLine && diagnostics.length) {
        state.selectedLine = diagnostics[0].line;
        scrollToLine(state.selectedLine);
        const firstCard = diagnosticList.querySelector('.diagnostic-item');
        if (firstCard) {
            firstCard.classList.add('active');
        }
    }
}

function applyPayload(payload) {
    state.payload = payload;
    state.selectedLine = null;
    state.passFilters.clear();

    const firstFileWithBug = (payload.files || []).find(file => file.bug_count > 0);
    state.selectedFilePath = firstFileWithBug?.path || payload.files?.[0]?.path || null;

    renderStats(payload);
    renderFilters(payload);
    refreshActiveFileView();
}

function resetResults() {
    state.payload = null;
    state.selectedFilePath = null;
    state.selectedLine = null;
    state.passFilters.clear();
    bugSummary.textContent = '0 个问题';
    fileSummary.textContent = '0 个文件';
    activeFileLabel.textContent = '当前文件：未选择';
    rawReportText.textContent = '';
    rawOutputText.textContent = '';
    fileList.className = 'file-list empty';
    fileList.innerHTML = '尚未上传工程';
    passFilters.className = 'pass-filters empty';
    passFilters.innerHTML = '暂无类型';
    diagnosticList.className = 'diagnostic-list empty';
    diagnosticList.innerHTML = '暂无结果';
    renderStats(null);
    renderCode(editor.value, []);
}

function fillEditorFromFile(file) {
    const reader = new FileReader();
    reader.onload = event => {
        editor.value = String(event.target?.result || '');
        filenameInput.value = file.name;
        projectZipInput.value = '';
        modePill.textContent = '单文件';
        renderCode(editor.value, []);
    };
    reader.readAsText(file, 'utf-8');
}

async function analyzeSource() {
    const code = editor.value;
    const filename = filenameInput.value.trim() || 'snippet.c';
    const singleFile = fileInput.files?.[0] || null;
    const projectZip = projectZipInput.files?.[0] || null;

    if (!projectZip && !singleFile && !code.trim()) {
        setStatus('error', '请先提供源码或工程');
        return;
    }

    setStatus('running', '诊断中...');
    analyzeButton.disabled = true;
    rawReportText.textContent = '';
    rawOutputText.textContent = '';

    const formData = new FormData();
    if (projectZip) {
        formData.append('project_zip', projectZip);
    } else if (singleFile) {
        formData.append('file', singleFile);
    } else {
        formData.append('code', code);
        formData.append('filename', filename);
    }

    try {
        const response = await fetch('/api/analyze', {
            method: 'POST',
            body: formData,
        });
        const payload = await response.json();
        if (!response.ok || !payload.ok) {
            setStatus('error', '诊断失败');
            resetResults();
            rawOutputText.textContent = [payload.error, payload.compile_output, payload.run_output].filter(Boolean).join('\n\n');
            return;
        }

        setStatus('success', payload.mode === 'project' ? '工程诊断完成' : '诊断完成');
        applyPayload(payload);
    } catch (error) {
        setStatus('error', '请求失败');
        resetResults();
        rawOutputText.textContent = String(error);
    } finally {
        analyzeButton.disabled = false;
    }
}

fileInput.addEventListener('change', event => {
    const [file] = event.target.files || [];
    if (file) {
        fillEditorFromFile(file);
    }
});

projectZipInput.addEventListener('change', event => {
    const [file] = event.target.files || [];
    if (file) {
        fileInput.value = '';
        modePill.textContent = `工程 zip：${file.name}`;
        setStatus('idle', '已选择 zip 工程');
    }
});

loadSampleButton.addEventListener('click', () => {
    editor.value = window.GRADUATION_SAMPLE_CODE || '';
    filenameInput.value = 'demo_null.c';
    fileInput.value = '';
    projectZipInput.value = '';
    modePill.textContent = '单文件';
    renderCode(editor.value, []);
    setStatus('idle', '已载入示例');
});

analyzeButton.addEventListener('click', () => {
    analyzeSource();
});

editor.addEventListener('input', () => {
    if (!state.payload || state.payload.mode !== 'single') {
        renderCode(editor.value, []);
        return;
    }
    const file = getSelectedFile();
    const diagnostics = getVisibleDiagnostics().filter(diag => diag.line <= editor.value.split('\n').length);
    if (file) {
        file.source = editor.value;
    }
    renderCode(editor.value, diagnostics, state.selectedLine);
});

resetResults();
