#include <iostream>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <algorithm>
#include <deque>
#include <memory>
#include <stack>

// Класс ThreadPool реализует пул потоков
class ThreadPool {
public:
	// Конструктор класса ThreadPool
	ThreadPool(size_t numThreads) : stop(false) {
		for (size_t i = 0; i < numThreads; ++i) {
			// Создаем и запускаем потоки в конструкторе
			workers.emplace_back([this] {
				while (true) {
					std::function<void()> task;
					{
						std::unique_lock<std::mutex> lock(queue_mutex);
						// Поток ожидает задачу в очереди или внешнюю задачу или сигнал завершения
						condition.wait(lock, [this] {
							return stop || !tasks.empty() || hasExternalTasks();
							});
						if (stop && tasks.empty() && !hasExternalTasks()) {
							return;
						}
						if (!tasks.empty()) {
							// Если есть задача в очереди, извлекаем её и выполняем
							task = std::move(tasks.front());
							tasks.pop_front();
						}
						else if (hasExternalTasks()) {
							// Если есть внешняя задача, извлекаем её и выполняем
							task = std::move(external_tasks.top());
							external_tasks.pop();
						}
					}
					if (task) {
						task();
					}
				}
				});
		}
	}

	// Метод enqueue добавляет задачу в очередь и возвращает std::future для результата
	template <typename F>
	auto enqueue(F&& f) -> std::future<typename std::result_of<F()>::type> {
		using return_type = typename std::result_of<F()>::type;
		auto task = std::make_shared<std::packaged_task<return_type()>>(std::forward<F>(f));
		std::future<return_type> res = task->get_future();
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			// Если пул потоков остановлен, выбрасываем исключение
			if (stop) {
				throw std::runtime_error("enqueue on stopped ThreadPool");
			}
			// Упаковываем задачу и добавляем её в очередь
			std::function<void()> wrapped_task = [task]() { (*task)(); };
			tasks.emplace_back(std::move(wrapped_task));
		}
		// Уведомляем один из потоков о наличии новой задачи
		condition.notify_one();
		return res;
	}

	// Деструктор класса ThreadPool
	~ThreadPool() {
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			stop = true; // Устанавливаем флаг завершения
		}
		condition.notify_all(); // Уведомляем все потоки о завершении
		for (std::thread& worker : workers) {
			worker.join(); // Дожидаемся завершения всех потоков
		}
	}

	// Метод wait позволяет дождаться завершения всех задач в очереди и внешних задач
	void wait() {
		std::unique_lock<std::mutex> lock(queue_mutex);
		condition.wait(lock, [this] {
			return tasks.empty() && !hasExternalTasks();
			});
	}

	// Метод externalEnqueue добавляет внешнюю задачу в очередь
	void externalEnqueue(std::function<void()> task) {
		std::unique_lock<std::mutex> lock(queue_mutex);
		external_tasks.push(std::move(task));
		condition.notify_one();
	}

	// Метод hasExternalTasks проверяет наличие внешних задач
	bool hasExternalTasks() const {
		return !external_tasks.empty();
	}

private:
	std::vector<std::thread> workers; // Потоки
	std::deque<std::function<void()>> tasks; // Очередь задач
	std::mutex queue_mutex; // Мьютекс для доступа к очереди
	std::condition_variable condition; // Условная переменная для ожидания задач
	bool stop; // Флаг завершения

	std::stack<std::function<void()>> external_tasks; // Стек внешних задач
};

// Функция quicksort выполняет сортировку методом быстрой сортировки
void quicksort(std::vector<int>& array, int left, int right, ThreadPool& pool) {
	if (left < right) {
		int middle = left + (right - left) / 2;
		int pivot = array[middle];
		int i = left, j = right;

		while (i <= j) {
			while (array[i] < pivot) {
				++i;
			}
			while (array[j] > pivot) {
				--j;
			}
			if (i <= j) {
				std::swap(array[i], array[j]);
				++i;
				--j;
			}
		}

		if (left < j) {
			if (j - left + 1 <= 100000) {
				// Сортируем в текущем потоке, так как размер подмассива небольшой
				quicksort(array, left, j, pool);
			}
			else {
				// Создаем подзадачу для сортировки левой части массива
				std::shared_ptr<std::promise<void>> promise = std::make_shared<std::promise<void>>();
				std::future<void> future = promise->get_future();
				pool.enqueue([&array, left, j, &pool, promise]() {
					quicksort(array, left, j, pool);
					promise->set_value();
					});
				future.wait();
			}
		}
		if (i < right) {
			if (right - i + 1 <= 100000) {
				// Сортируем в текущем потоке, так как размер подмассива небольшой
				quicksort(array, i, right, pool);
			}
			else {
				// Создаем подзадачу для сортировки правой части массива
				pool.enqueue([&array, i, right, &pool]() {
					quicksort(array, i, right, pool);
					});
			}
		}
	}
}