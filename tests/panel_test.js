const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const listeners = {};
const calls = [];
const document = {
    addEventListener: (name, fn) => { listeners[name] = fn; },
    open: () => calls.push('open'),
    write: html => calls.push(html),
    close: () => calls.push('close')
};
const window = {
    neoGpuTimer: 42,
    close: () => calls.push('window.close'),
    location: {replace: href => calls.push(href)}
};
vm.runInNewContext(fs.readFileSync('assets/panel.js', 'utf8'), {
    document, window, URLSearchParams,
    FormData: class { constructor(form) { return form.fields; } },
    clearInterval: id => calls.push(id),
    alert: text => { throw Error(text); },
    fetch: async (url, options) => {
        calls.push([url, options.method, options.body]);
        return {text: async () => '<html>updated</html>'};
    }
});
(async () => {
    for (const mhz of ['2150', '2350']) {
        // A replaced document registers fresh listeners.
        listeners.DOMContentLoaded();
        const form = {method: 'post', fields: [['mhz', mhz]], action: '/'};
        const submitter = {form, hasAttribute: () => true, formAction: '/gpu/enable'};
        const event = {target: form, submitter, preventDefault: () => calls.push('prevent')};
        listeners.submit(event);
        listeners.submit(event); // A double press cannot submit twice while pending.
        await new Promise(resolve => setImmediate(resolve));
    }
    assert.deepEqual(calls.filter(Array.isArray), [
        ['/gpu/enable', 'POST', 'mhz=2150'], ['/gpu/enable', 'POST', 'mhz=2350']
    ]);
    assert.equal(calls.filter(x => x === 'open').length, 2);
    listeners.click({target: {closest: s => s === 'nav a' ? {href: '/gpu'} : null}, preventDefault() {}});
    assert(calls.includes('/gpu'));
    listeners.keydown({keyCode: 27});
    listeners.keydown({key: 'BrowserBack'});
    assert.equal(calls.filter(x => x === 'window.close').length, 2);
    console.log('PASS actions avoid POST navigation, double-submit guard, tab replacement and close keys');
})();
