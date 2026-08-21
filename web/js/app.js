async function api(url, options) {
    try {
        const res = await fetch(url, options);
        const text = await res.text();
        return { status: res.status, text };
    } catch (e) {
        return { status: 'ERR', text: String(e) };
    }
}

function render(el, { status, text }) {
    el.textContent = `HTTP ${status}\n\n${text}`;
}

document.getElementById('btn-time').addEventListener('click', async () => {
    const out = document.getElementById('out-time');
    out.textContent = '请求中…';
    render(out, await api('/api/time'));
});

document.getElementById('btn-echo').addEventListener('click', async () => {
    const out = document.getElementById('out-echo');
    const val = document.getElementById('echo-input').value;
    out.textContent = '请求中…';
    render(out, await api('/api/echo', {
        method: 'POST',
        body: val,
        headers: { 'Content-Type': 'text/plain' },
    }));
});

// 接口卡片：POST 卡片点击后跳到下方体验区并聚焦输入框
document.getElementById('card-echo').addEventListener('click', () => {
    const input = document.getElementById('echo-input');
    input.scrollIntoView({ behavior: 'smooth', block: 'center' });
    input.focus();
});
