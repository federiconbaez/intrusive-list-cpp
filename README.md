# intrusive-list-cpp

Lista doblemente enlazada intrusiva en C++20, header-only: los enlaces viven dentro del elemento (un hook como clase base), así que insertar, sacar, borrar y hacer splice nunca reservan memoria. Un mismo objeto puede estar en varias listas a la vez usando tags distintos.

Header-only intrusive doubly linked list in C++20: the links live inside the element (a base-class hook), so push, pop, erase and splice never allocate. One object can be in several lists at once through different tags.

```cpp
struct run_tag {};
struct job : ilist::hook<>, ilist::hook<run_tag> { int prio; };

ilist::list<job> all;            // every job
ilist::list<job, run_tag> ready; // the runnable ones, same objects
job j{.prio = 3};
all.push_back(j);
ready.push_back(j);
ready.remove(j);                 // O(1), no search
all.clear();                     // unlinks; never frees anything
```

## Español

**Qué ofrece**

- `push_front/back`, `pop_front/back`, `insert`, `erase`, `remove(T&)` e `iterator_to(T&)` en O(1).
- `splice` de lista entera y de un elemento en O(1); de un rango en O(1) dentro de la misma lista y O(k) entre listas en modo `safe`, para mantener `size()` exacto.
- `sort` estable (merge sort bottom-up, sin recursión ni memoria extra).
- Iteradores bidireccionales: cumple `std::bidirectional_iterator` y `std::ranges::bidirectional_range`, funciona con `std::ranges` y `views::reverse`.
- Dos modos de enlace:
  - `safe` (por defecto): `size()` O(1); destruir un elemento que sigue enlazado **aborta** con un mensaje, en vez de dejar un nodo colgando.
  - `auto_unlink`: el destructor del elemento lo saca de la lista (útil cuando el dueño del objeto no conoce la lista); `size()` pasa a ser O(n).
- Copiar o mover un elemento no copia su pertenencia a listas. El destructor de la lista desenlaza todo y no toca los objetos.
- El hook ocupa dos punteros por tag. La conversión hook → elemento es un `static_cast` de base a derivada; no hay aritmética con offsets.

**Pruebas** (`ctest`; g++ 14 y clang 19, con ASan+UBSan, con TSan y en Release)

- Operaciones básicas, recorrido inverso, algoritmos de `std::ranges`, un objeto en dos listas.
- `splice` en todas sus formas: lista entera al medio, un elemento entre listas, rango entre listas, rango dentro de la misma lista, no-ops sobre sí mismo; constructor y asignación por movimiento.
- `sort` contra `std::stable_sort` en tamaños 0 a 4097 con claves repetidas (verifica estabilidad y los punteros `prev` reconstruidos) y sin asignaciones.
- Modelo: 200 000 operaciones aleatorias en dos listas contra `std::list<node*>`, verificando cada enlace y el tamaño periódicamente.
- **Cero asignaciones**: un `operator new` global contador rodea 100 rondas de splice, pop/push y rangos sobre 1000 elementos, más un sort (apagado en builds con TSan, cuyo runtime define su propio `operator new`).
- Salida de scope: elementos `auto_unlink` destruidos dentro de la lista (en la pila y en el heap) la dejan consistente.
- Modo `safe`: un proceso hijo destruye un elemento enlazado y debe morir con `SIGABRT`.
- Hilos: 4 hilos mueven 256 nodos entre dos listas bajo un mutex, 20 000 veces cada uno; TSan sin reportes y todos los nodos contabilizados.
- Mutaciones de control: sort no estable, un `prev` sin actualizar en el splice de rango y un tamaño mal contado entre listas hacen fallar los tests.

**Rendimiento** (`bench_list`, g++ 14 `-O3`, VM Intel Xeon de 8 vCPU, 100 000 elementos de 64 bytes, mediana de 7, ns por operación):

| operación | ilist | `std::list<T>` | `std::list<T*>` |
|---|---|---|---|
| cola: `pop_front` + `push_back` | 2.8 | 14.1 | 13.5 |
| recorrer, orden de lista = orden en memoria | 3.5 | 10.1 | 11.0 |
| recorrer, nodos desordenados en memoria | 35.6 | 7.3 | 14.3 |
| `sort` por clave (por elemento) | 675 | 285 | 376 |

Lectura honesta: la intrusiva gana cuando el costo es asignar (cola) o cuando el orden de la lista sigue a la memoria. Pierde cuando los objetos están dispersos: `std::list<T>` **copia** los objetos a nodos recién asignados y contiguos, así que recorre memoria secuencial; la intrusiva recorre los objetos donde están. El `sort` bottom-up hace log n pasadas persiguiendo enlaces por esa memoria dispersa y queda ~2,4× más lento que el de libstdc++.

**Límites**: no es thread-safe (como los contenedores estándar); `remove`, `iterator_to` y `splice` de un elemento confían en que el elemento esté en esa lista (no se verifica); en modo `auto_unlink` no hay `size()` O(1).

## English

**What it offers**

- `push_front/back`, `pop_front/back`, `insert`, `erase`, `remove(T&)` and `iterator_to(T&)` in O(1).
- Whole-list and single-element `splice` in O(1); range `splice` in O(1) within one list and O(k) across lists in `safe` mode, to keep `size()` exact.
- Stable `sort` (bottom-up merge sort, no recursion, no extra memory).
- Bidirectional iterators: satisfies `std::bidirectional_iterator` and `std::ranges::bidirectional_range`, works with `std::ranges` and `views::reverse`.
- Two link modes:
  - `safe` (default): O(1) `size()`; destroying an element that is still linked **aborts** with a message instead of leaving a dangling node.
  - `auto_unlink`: the element's destructor unlinks it (handy when the object's owner does not know the list); `size()` becomes O(n).
- Copying or moving an element does not copy its list membership. The list destructor unlinks everything and never touches the objects.
- The hook is two pointers per tag. Hook → element is a base-to-derived `static_cast`; there is no offset arithmetic.

**Tests** (`ctest`; g++ 14 and clang 19, with ASan+UBSan, with TSan and in Release)

- Basic operations, reverse traversal, `std::ranges` algorithms, one object in two lists.
- Every form of `splice`: whole list into the middle, one element across lists, a range across lists, a range within one list, self no-ops; move construction and assignment.
- `sort` against `std::stable_sort` for sizes 0 to 4097 with repeated keys (checks stability and the rebuilt `prev` pointers) and with no allocations.
- Model: 200,000 random operations on two lists against `std::list<node*>`, checking every link and the size periodically.
- **Zero allocations**: a counting global `operator new` brackets 100 rounds of splice, pop/push and ranges over 1000 elements, plus a sort (off in TSan builds, whose runtime defines its own `operator new`).
- Scope exit: `auto_unlink` elements destroyed while linked (on the stack and on the heap) leave the list consistent.
- `safe` mode: a child process destroys a linked element and must die with `SIGABRT`.
- Threads: 4 threads move 256 nodes between two lists under a mutex, 20,000 times each; TSan clean and every node accounted for.
- Control mutations: an unstable sort, a missing `prev` update in range splice and a miscounted cross-list size all make the tests fail.

**Performance**: see the table above (`bench_list`, g++ 14 `-O3`, 8-vCPU Intel Xeon VM, 100,000 elements of 64 bytes, median of 7, ns per operation).

Honest reading: the intrusive list wins when the cost is allocation (queue churn) or when list order follows memory order. It loses when objects are scattered: `std::list<T>` **copies** the objects into freshly allocated, contiguous nodes, so it walks sequential memory; the intrusive list walks the objects where they live. The bottom-up `sort` makes log n passes chasing links through that scattered memory and ends up ~2.4× slower than libstdc++'s.

**Limits**: not thread-safe (like the standard containers); `remove`, `iterator_to` and single-element `splice` trust that the element is in that list (not checked); no O(1) `size()` in `auto_unlink` mode.

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DILIST_SANITIZE=address,undefined   # or thread, or empty
cmake --build build && ctest --test-dir build
./build/bench_list   # use a Release build
```

## License

Apache-2.0
