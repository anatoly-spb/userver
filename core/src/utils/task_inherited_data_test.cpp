#include <utest/utest.hpp>

#include <engine/condition_variable.hpp>
#include <engine/mutex.hpp>
#include <utils/async.hpp>
#include <utils/task_inherited_data.hpp>

TEST(TaskInheritedData, Empty) {
  RunInCoro([] {
    const std::string key = "key";

    EXPECT_EQ(utils::GetTaskInheritedDataOptional<std::string>(key), nullptr);
    EXPECT_THROW(utils::GetTaskInheritedData<std::string>(key),
                 std::runtime_error);
  });
}

TEST(TaskInheritedData, MoveAndShared) {
  RunInCoro([] {
    class Data {
     public:
      Data() = default;
      Data(std::string data) : data_(std::move(data)) {}
      Data(const Data&) = delete;
      Data(Data&&) = default;

      const std::string& GetData() const { return data_; }

     private:
      std::string data_{};
    };

    const std::string key = "key";
    const std::string value = "value";
    Data data(value);

    EXPECT_EQ(utils::GetTaskInheritedDataOptional<Data>(key), nullptr);

    utils::SetTaskInheritedData(key, std::move(data));

    auto data_opt = utils::GetTaskInheritedDataOptional<Data>(key);
    ASSERT_NE(data_opt, nullptr);
    EXPECT_EQ(data_opt->GetData(), value);

    EXPECT_EQ(utils::GetTaskInheritedData<Data>(key).GetData(), value);

    auto sub_task = utils::Async("subtask", [&]() {
      auto data_opt = utils::GetTaskInheritedDataOptional<Data>(key);
      ASSERT_NE(data_opt, nullptr);
      EXPECT_EQ(data_opt->GetData(), value);

      EXPECT_EQ(utils::GetTaskInheritedData<Data>(key).GetData(), value);
    });
    sub_task.Get();
  });
}

TEST(TaskInheritedData, IndependenceFromChildChanges) {
  RunInCoro([] {
    const std::string key1 = "key1";
    const std::string key2 = "key2";
    const std::string key3 = "key3";
    const std::string value1 = "value1";
    const std::string value2 = "value2";
    const std::string value3 = "value3";

    utils::SetTaskInheritedData(key1, value1);
    utils::SetTaskInheritedData(key2, value2);

    auto check_data_unchanged = [&]() {
      EXPECT_EQ(utils::GetTaskInheritedData<std::string>(key1), value1);
      EXPECT_EQ(utils::GetTaskInheritedData<std::string>(key2), value2);
      EXPECT_THROW(utils::GetTaskInheritedData<std::string>(key3),
                   std::runtime_error);

      ASSERT_NE(utils::GetTaskInheritedDataOptional<std::string>(key1),
                nullptr);
      EXPECT_EQ(*utils::GetTaskInheritedDataOptional<std::string>(key1),
                value1);
      ASSERT_NE(utils::GetTaskInheritedDataOptional<std::string>(key2),
                nullptr);
      EXPECT_EQ(*utils::GetTaskInheritedDataOptional<std::string>(key2),
                value2);
      EXPECT_EQ(utils::GetTaskInheritedDataOptional<std::string>(key3),
                nullptr);
    };

    check_data_unchanged();

    auto sub_task = utils::Async("subtask", [&]() {
      const std::string new_value1 = "new_value1";

      // Test inheritance
      check_data_unchanged();

      utils::SetTaskInheritedData(key1, new_value1);
      utils::EraseTaskInheritedData(key2);
      utils::SetTaskInheritedData(key3, value3);

      EXPECT_EQ(utils::GetTaskInheritedData<std::string>(key1), new_value1);
      EXPECT_THROW(utils::GetTaskInheritedData<std::string>(key2),
                   std::runtime_error);
      EXPECT_EQ(utils::GetTaskInheritedData<std::string>(key3), value3);

      ASSERT_NE(utils::GetTaskInheritedDataOptional<std::string>(key1),
                nullptr);
      EXPECT_EQ(*utils::GetTaskInheritedDataOptional<std::string>(key1),
                new_value1);
      EXPECT_EQ(utils::GetTaskInheritedDataOptional<std::string>(key2),
                nullptr);
      ASSERT_NE(utils::GetTaskInheritedDataOptional<std::string>(key3),
                nullptr);
      EXPECT_EQ(*utils::GetTaskInheritedDataOptional<std::string>(key3),
                value3);
    });

    sub_task.Get();

    // Subtask does not change data in parent task.
    check_data_unchanged();
  });
}

TEST(TaskInheritedData, IndependenceFromParentChanges) {
  RunInCoro([] {
    const std::string key1 = "key1";
    const std::string key2 = "key2";
    const std::string key3 = "key3";
    const std::string value1 = "value1";
    const std::string value2 = "value2";
    const std::string value3 = "value3";
    const std::string new_value1 = "new_value1";

    engine::Mutex mutex;
    engine::ConditionVariable cv;
    bool signaled = false;

    utils::SetTaskInheritedData(key1, value1);
    utils::SetTaskInheritedData(key2, value2);

    auto check_data_unchanged = [&]() {
      EXPECT_EQ(utils::GetTaskInheritedData<std::string>(key1), value1);
      EXPECT_EQ(utils::GetTaskInheritedData<std::string>(key2), value2);
      EXPECT_THROW(utils::GetTaskInheritedData<std::string>(key3),
                   std::runtime_error);

      ASSERT_NE(utils::GetTaskInheritedDataOptional<std::string>(key1),
                nullptr);
      EXPECT_EQ(*utils::GetTaskInheritedDataOptional<std::string>(key1),
                value1);
      ASSERT_NE(utils::GetTaskInheritedDataOptional<std::string>(key2),
                nullptr);
      EXPECT_EQ(*utils::GetTaskInheritedDataOptional<std::string>(key2),
                value2);
      EXPECT_EQ(utils::GetTaskInheritedDataOptional<std::string>(key3),
                nullptr);
    };

    check_data_unchanged();

    auto sub_task = utils::Async("subtask", [&]() {
      // Test inheritance
      check_data_unchanged();

      {
        std::unique_lock<engine::Mutex> lock(mutex);
        EXPECT_TRUE(cv.Wait(lock, [&signaled]() { return signaled; }));
      }

      // Data in subtask does not change if parent task data was changed.
      check_data_unchanged();
    });

    utils::SetTaskInheritedData(key1, new_value1);
    utils::EraseTaskInheritedData(key2);
    utils::SetTaskInheritedData(key3, value3);

    EXPECT_EQ(utils::GetTaskInheritedData<std::string>(key1), new_value1);
    EXPECT_THROW(utils::GetTaskInheritedData<std::string>(key2),
                 std::runtime_error);
    EXPECT_EQ(utils::GetTaskInheritedData<std::string>(key3), value3);

    ASSERT_NE(utils::GetTaskInheritedDataOptional<std::string>(key1), nullptr);
    EXPECT_EQ(*utils::GetTaskInheritedDataOptional<std::string>(key1),
              new_value1);
    EXPECT_EQ(utils::GetTaskInheritedDataOptional<std::string>(key2), nullptr);
    ASSERT_NE(utils::GetTaskInheritedDataOptional<std::string>(key3), nullptr);
    EXPECT_EQ(*utils::GetTaskInheritedDataOptional<std::string>(key3), value3);

    {
      std::unique_lock<engine::Mutex> lock(mutex);
      signaled = true;
    }
    cv.NotifyAll();

    sub_task.Get();
  });
}

#ifdef NDEBUG
#define EXPECT_THROW_RELEASE_DEATH_DEBUG(statement, expected_exception) \
  EXPECT_THROW(statement, expected_exception)
#else
#define EXPECT_THROW_RELEASE_DEATH_DEBUG(statement, expected_exception) \
  EXPECT_DEATH(statement, "");
#endif

TEST(TaskInheritedDataDeathTest, TypeMismatch) {
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  RunInCoro([] {
    const std::string key = "key";
    const std::string value = "value";

    utils::SetTaskInheritedData(key, value);

    auto data_opt = utils::GetTaskInheritedDataOptional<std::string>(key);
    ASSERT_NE(data_opt, nullptr);
    EXPECT_EQ(*data_opt, value);

    EXPECT_THROW_RELEASE_DEATH_DEBUG(
        utils::GetTaskInheritedDataOptional<int>(key), std::bad_any_cast);
    EXPECT_THROW_RELEASE_DEATH_DEBUG(utils::GetTaskInheritedData<int>(key),
                                     std::bad_any_cast);

    utils::EraseTaskInheritedData(key);

    EXPECT_EQ(utils::GetTaskInheritedDataOptional<int>(key), nullptr);
    EXPECT_THROW(utils::GetTaskInheritedData<int>(key), std::runtime_error);
  });
}

TEST(TaskInheritedDataDeathTest, Overwrite) {
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  RunInCoro([] {
    const std::string key = "key";
    const std::string value = "value";

    utils::SetTaskInheritedData(key, value);

    auto data_opt = utils::GetTaskInheritedDataOptional<std::string>(key);
    ASSERT_NE(data_opt, nullptr);
    EXPECT_EQ(*data_opt, value);

    utils::SetTaskInheritedData(key, 42);

    EXPECT_NE(utils::GetTaskInheritedDataOptional<int>(key), nullptr);
    EXPECT_EQ(*utils::GetTaskInheritedDataOptional<int>(key), 42);
    EXPECT_EQ(utils::GetTaskInheritedData<int>(key), 42);

    EXPECT_THROW_RELEASE_DEATH_DEBUG(
        utils::GetTaskInheritedDataOptional<std::string>(key),
        std::bad_any_cast);
    EXPECT_THROW_RELEASE_DEATH_DEBUG(
        utils::GetTaskInheritedData<std::string>(key), std::bad_any_cast);

    utils::EraseTaskInheritedData(key);

    EXPECT_EQ(utils::GetTaskInheritedDataOptional<int>(key), nullptr);
    EXPECT_THROW(utils::GetTaskInheritedData<int>(key), std::runtime_error);
    EXPECT_EQ(utils::GetTaskInheritedDataOptional<std::string>(key), nullptr);
    EXPECT_THROW(utils::GetTaskInheritedData<std::string>(key),
                 std::runtime_error);
  });
}
